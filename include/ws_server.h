#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// WsServer – WebSocket server for the HV daemon
//
// Uses websocketpp with standalone Asio (no Boost).
// Runs on the main thread via asio::io_context.
//
// Responsibilities:
//   - Accept WebSocket client connections
//   - Periodically check SnapshotStore for new data and broadcast
//   - Receive JSON commands from clients and push to CommandQueue
//   - Serve static config files on initial connect (module geometry, etc.)
//   - Enforce three-tier access control (Guest / User / Expert)
// ─────────────────────────────────────────────────────────────────────────────

#ifndef ASIO_STANDALONE
#define ASIO_STANDALONE
#endif

#include <websocketpp/config/asio_no_tls.hpp>
#include <websocketpp/extensions/permessage_deflate/enabled.hpp>
#include <websocketpp/server.hpp>
#include <nlohmann/json.hpp>

#include "hv_daemon.h"
#include "file_op_logger.h"

#include <map>
#include <set>
#include <string>
#include <vector>
#include <iostream>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <functional>
#include <chrono>
#include <ctime>
#include <cstdio>
#include <optional>
#include <sys/stat.h>

using json = nlohmann::json;

// ─────────────────────────────────────────────────────────────────────────────
// Auto-report config block (read from gui_config.json["auto_report"]).
//
// The dispatcher lives inside WsServer; setAutoReportConfig() takes a fully-
// populated struct and probes local_save_dir at startup.  When enabled +
// scheduled_interval_s > 0, the existing 100 ms broadcast timer fires a
// capture_request to one reporter-capable client every interval_s seconds.
// The chosen client screenshots its tabs, builds the elog XML, and POSTs to
// /api/elog/post; the daemon mirrors the body on disk and (if post_to_elog)
// uploads via curl --cert --key --upload-file.
// ─────────────────────────────────────────────────────────────────────────────
struct AutoReportConfig {
    bool        enabled              = false;
    bool        post_to_elog         = false;
    std::string local_save_dir;
    int         min_interval_ms      = 600000;   // 10 min
    int         scheduled_interval_s = 0;        // 0 = manual replay only
    std::string elog_url;
    std::string elog_logbook;
    std::string elog_author;
    std::vector<std::string> elog_tags;
    std::string elog_cert;
    std::string elog_key;
};

// ── Custom websocketpp config: Asio (no TLS) + permessage-deflate ────
// The hv_snapshot JSON for ~1700 channels is 400–600 KB of highly
// repetitive text.  permessage-deflate compresses it ~10× on the wire,
// which eliminates the periodic micro-freeze when tunnelling over SSH.
// Browsers negotiate the extension automatically.
struct asio_deflate_config : public websocketpp::config::asio {
    typedef websocketpp::extensions::permessage_deflate::enabled
        <asio_deflate_config> permessage_deflate_type;
};

using WsServerType = websocketpp::server<asio_deflate_config>;
using connection_hdl = websocketpp::connection_hdl;


class WsServer
{
public:
    WsServer(uint16_t port,
             SnapshotStore &store,
             CommandQueue  &cmdq,
             const std::string &resource_dir    = "",
             const std::string &module_geo_path = "",
             const std::string &gui_config_path = "",
             const std::string &user_password   = "",
             const std::string &expert_password = "",
             FileOpLogger      *op_logger       = nullptr,
             HVAggregator      *aggregator      = nullptr)
        : port_(port), store_(store), cmdq_(cmdq),
          resource_dir_(resource_dir),
          user_pass_(user_password),
          expert_pass_(expert_password),
          op_logger_(op_logger),
          aggregator_(aggregator)
    {
        // Pre-load static config files into memory
        module_geo_json_ = readFile(module_geo_path, "[]");
        gui_config_json_ = readFile(gui_config_path, "{}");

        // Configure websocketpp
        server_.set_access_channels(websocketpp::log::alevel::none);
        server_.set_error_channels(websocketpp::log::elevel::warn |
                                   websocketpp::log::elevel::rerror |
                                   websocketpp::log::elevel::fatal);

        server_.init_asio();
        server_.set_reuse_addr(true);

        server_.set_open_handler(
            [this](connection_hdl hdl) { onOpen(hdl); });
        server_.set_close_handler(
            [this](connection_hdl hdl) { onClose(hdl); });
        server_.set_message_handler(
            [this](connection_hdl hdl, WsServerType::message_ptr msg) {
                onMessage(hdl, msg);
            });

        // Serve static files for plain HTTP requests (non-WebSocket).
        // This lets browsers load the dashboard directly from
        // http://host:port/ without a separate file server.
        server_.set_http_handler(
            [this](connection_hdl hdl) { onHttp(hdl); });
    }

    // Blocks on the Asio event loop.  Call stop() from a signal handler
    // or another thread to break out.
    void run()
    {
        server_.listen(port_);
        server_.start_accept();

        std::cout << fmt::format("WebSocket + HTTP server listening on port {}\n", port_);
        if (!resource_dir_.empty())
            std::cout << fmt::format("  Dashboard: http://localhost:{}/\n", port_);
        if (authRequired())
            std::cout << "  Access control: ENABLED (passwords configured)\n";
        else
            std::cout << "  Access control: DISABLED (no passwords — full access for all)\n";

        // Set up a recurring timer to broadcast snapshots
        scheduleBroadcast();

        server_.run();
    }

    void stop()
    {
        server_.stop_listening();
        // Close all connections gracefully
        for (auto &[hdl, info] : connections_) {
            try {
                server_.close(hdl, websocketpp::close::status::going_away,
                              "daemon shutting down");
            } catch (...) {}
        }
        server_.stop();
    }

    size_t clientCount() const { return connections_.size(); }

    // Apply the auto_report block parsed from gui_config.json.  Probes
    // local_save_dir for writability and prints a one-line summary so the
    // operator sees at startup whether reports will be archived.  Safe to
    // call before run(); the timer thread reads auto_report_cfg_ from the
    // same asio thread that we're on here.
    void setAutoReportConfig(AutoReportConfig cfg)
    {
        auto_report_cfg_ = std::move(cfg);
        checkSaveDirWritable();
        std::cerr << "AutoReport: enabled="
                  << (auto_report_cfg_.enabled ? "ON" : "OFF")
                  << " post_to_elog=" << (auto_report_cfg_.post_to_elog ? "yes" : "no")
                  << " min_interval_ms=" << auto_report_cfg_.min_interval_ms
                  << " scheduled_interval_s=" << auto_report_cfg_.scheduled_interval_s
                  << (auto_report_cfg_.local_save_dir.empty()
                       ? std::string()
                       : " local_save=" + auto_report_cfg_.local_save_dir)
                  << "\n";
        if (!auto_report_cfg_.elog_url.empty()) {
            std::cerr << "Elog      : " << auto_report_cfg_.elog_url
                      << " logbook=" << auto_report_cfg_.elog_logbook
                      << (auto_report_cfg_.elog_cert.empty() ? ""
                           : " cert=" + auto_report_cfg_.elog_cert)
                      << "\n";
        }
    }

private:
    // ── Per-connection state ─────────────────────────────────────────────

    struct ClientInfo {
        int accessLevel = 0;  // 0=guest, 1=user, 2=expert
    };

    // True if at least one password is configured
    bool authRequired() const { return !user_pass_.empty() || !expert_pass_.empty(); }

    // ── Connection lifecycle ─────────────────────────────────────────────

    void onOpen(connection_hdl hdl)
    {
        // Default: guest if passwords configured, full expert otherwise
        ClientInfo info;
        info.accessLevel = authRequired() ? 0 : 2;
        connections_[hdl] = info;

        std::cout << fmt::format("Client connected ({} total, level={})\n",
                                 connections_.size(), info.accessLevel);

        // Send static config to the new client immediately
        json init;
        init["type"]               = "init";
        init["module_geometry"]    = json::parse(module_geo_json_, nullptr, false);
        init["gui_config"]         = json::parse(gui_config_json_, nullptr, false);
        init["fault_log_capacity"] = store_.faultLogCapacity();
        init["auth_required"]      = authRequired();
        init["access_level"]       = info.accessLevel;
        init["auto_report"]        = autoReportClientView();
        // Full aggregation history — new client needs this to draw the
        // Aggregated plot immediately instead of waiting for the next flush.
        if (aggregator_) {
            init["hv_aggregation"] = json::parse(aggregator_->fullSnapshotJson(),
                                                  nullptr, false);
        } else {
            init["hv_aggregation"] = json::array();
        }

        try {
            server_.send(hdl, init.dump(), websocketpp::frame::opcode::text);
        } catch (const std::exception &e) {
            std::cerr << "Failed to send init to client: " << e.what() << "\n";
        }

        // Also send current snapshots right away
        sendSnapshot(hdl, "hv_snapshot",      store_.getHV().first);
        sendSnapshot(hdl, "board_snapshot",   store_.getBoard().first);
        sendSnapshot(hdl, "booster_snapshot", store_.getBooster().first);
        sendSnapshot(hdl, "crate_status",     store_.getCrateStatus().first);

        // Send full fault logs (two separate buffers) and advance versions
        auto [flf_data, flf_ver] = store_.getFaultLogFaults();
        sendSnapshot(hdl, "fault_log_faults", flf_data);
        if (flf_ver > last_flf_ver_)
            last_flf_ver_ = flf_ver;

        auto [flw_data, flw_ver] = store_.getFaultLogWarns();
        sendSnapshot(hdl, "fault_log_warns", flw_data);
        if (flw_ver > last_flw_ver_)
            last_flw_ver_ = flw_ver;
    }

    void onClose(connection_hdl hdl)
    {
        connections_.erase(hdl);
        reporter_capable_.erase(hdl);
        std::cout << fmt::format("Client disconnected ({} remaining)\n",
                                 connections_.size());
    }

    void onMessage(connection_hdl hdl, WsServerType::message_ptr msg)
    {
        try {
            json cmd = json::parse(msg->get_payload());
            std::string type = cmd.value("type", "");

            // ── Capability advertisement (auto-report eligibility) ───────
            // A reporter-capable client opts into the dispatcher pool by
            // sending {"type":"client_hello","capabilities":["auto_report"]}
            // after the WS opens.  Tabs that don't ship report.js never
            // send this and stay out of the pool, so the watchdog never
            // burns 30 s timing out a peer that wouldn't know what to do
            // with a capture_request.
            if (type == "client_hello") {
                bool can_capture = false;
                if (cmd.contains("capabilities") &&
                    cmd["capabilities"].is_array())
                {
                    for (const auto &c : cmd["capabilities"]) {
                        if (c.is_string() &&
                            c.get<std::string>() == "auto_report")
                        { can_capture = true; break; }
                    }
                }
                if (can_capture) reporter_capable_.insert(hdl);
                json hello = {{"type", "server_hello"},
                              {"capabilities", json::array({"auto_report"})}};
                try {
                    server_.send(hdl, hello.dump(),
                                 websocketpp::frame::opcode::text);
                } catch (...) {}
                return;
            }

            // ── Authentication request (handled directly, not queued) ────
            if (type == "auth") {
                handleAuth(hdl, cmd);
                return;
            }

            // ── Access control gating ───────────────────────────────────
            int level = 0;
            auto it = connections_.find(hdl);
            if (it != connections_.end())
                level = it->second.accessLevel;

            // Power commands require level >= 1 (User)
            if (type == "set_power" || type == "set_all_power" ||
                type == "set_power_batch" ||
                type == "booster_set_output")
            {
                if (level < 1) {
                    sendError(hdl, "access_denied",
                              "Power control requires User access or higher");
                    return;
                }
            }

            // Parameter edits + load require level >= 2 (Expert)
            if (type == "set_voltage" || type == "set_current" ||
                type == "set_svmax"   || type == "set_name"    ||
                type == "set_all_voltage" ||
                type == "set_voltage_by_name" ||
                type == "load_settings" ||
                type == "booster_set_voltage" || type == "booster_set_current")
            {
                if (level < 2) {
                    sendError(hdl, "access_denied",
                              "Parameter editing requires Expert access");
                    return;
                }
            }

            // save_settings / get_voltage (read-only) are ungated — safe for all levels

            // ── Read-only queries (handled directly, not queued) ────────
            if (type == "get_voltage") {
                handleGetVoltage(hdl, cmd);
                return;
            }

            // ── Log accepted operations to file ─────────────────────────
            if (op_logger_) op_logger_->logCommand(level, cmd);

            // save_settings / load_settings: route to HV queue, remember who asked
            if (type == "save_settings") {
                std::lock_guard lk(settings_mu_);
                settings_requester_ = hdl;
                settings_pending_ = true;
                cmdq_.push({ Command::HV, std::move(cmd) });
                return;
            }
            if (type == "load_settings") {
                std::lock_guard lk(settings_mu_);
                load_requester_ = hdl;
                load_pending_ = true;
                cmdq_.push({ Command::HV, std::move(cmd) });
                return;
            }

            // Route to the correct target
            if (type.rfind("booster_", 0) == 0 ||
                (type == "set_poll_interval" && cmd.contains("target") &&
                 cmd["target"] == "booster"))
            {
                cmdq_.push({ Command::Booster, std::move(cmd) });
            } else {
                cmdq_.push({ Command::HV, std::move(cmd) });
            }
        } catch (const json::parse_error &e) {
            std::cerr << "Invalid JSON from client: " << e.what() << "\n";
        }
    }

    // ── Authentication handler ───────────────────────────────────────────

    void handleAuth(connection_hdl hdl, const json &cmd)
    {
        int requested = cmd.value("level", 0);
        std::string pw = cmd.value("password", "");

        int granted = 0;
        std::string reason;

        if (requested <= 0) {
            // Guest — always succeeds (also used for "log out")
            granted = 0;
        }
        else if (!authRequired()) {
            // No passwords configured — grant whatever is requested
            granted = std::min(requested, 2);
        }
        else if (requested == 1) {
            // User level — either password grants at least user access
            if (pw == user_pass_ || (!expert_pass_.empty() && pw == expert_pass_)) {
                granted = 1;
            } else {
                reason = "incorrect password";
            }
        }
        else {
            // Expert level
            if (!expert_pass_.empty() && pw == expert_pass_) {
                granted = 2;
            } else if (expert_pass_.empty() && pw == user_pass_) {
                // No separate expert password — user password grants expert
                granted = 2;
            } else {
                reason = "incorrect password";
            }
        }

        connections_[hdl].accessLevel = granted;

        json resp;
        resp["type"]      = "auth_result";
        resp["granted"]   = granted;
        resp["requested"] = requested;
        if (!reason.empty()) resp["reason"] = reason;

        try {
            server_.send(hdl, resp.dump(), websocketpp::frame::opcode::text);
        } catch (...) {}

        const char *labels[] = { "Guest", "User", "Expert" };
        std::cout << fmt::format("Auth: client requested={}, granted={} ({})\n",
                                 labels[std::min(requested, 2)],
                                 labels[std::min(granted, 2)],
                                 reason.empty() ? "ok" : reason);

        if (op_logger_) op_logger_->logAuth(requested, granted, reason);
    }

    // ── Error response helper ────────────────────────────────────────────

    // ── get_voltage: read-only query, returns channel info by name ────────

    void handleGetVoltage(connection_hdl hdl, const json &cmd)
    {
        std::string name = cmd.value("name", "");
        if (name.empty()) {
            sendError(hdl, "invalid_request", "get_voltage requires a 'name' field");
            return;
        }

        // Search the latest HV snapshot for the named channel
        auto [snap, ver] = store_.getHV();
        json channels = json::parse(snap, nullptr, false);
        if (!channels.is_array()) {
            sendError(hdl, "no_data", "No HV snapshot available yet");
            return;
        }

        for (const auto &ch : channels) {
            if (ch.value("name", "") == name) {
                json resp;
                resp["type"] = "get_voltage_response";
                for (const char *key : {"name","crate","slot","channel",
                                        "vset","vmon","limit","iset","imon",
                                        "on","status"}) {
                    if (ch.contains(key)) resp[key] = ch[key];
                }
                try {
                    server_.send(hdl, resp.dump(),
                                 websocketpp::frame::opcode::text);
                } catch (...) {}
                return;
            }
        }

        sendError(hdl, "not_found",
                  "Channel '" + name + "' not found");
    }

    void sendError(connection_hdl hdl, const std::string &code,
                   const std::string &message)
    {
        json err;
        err["type"]    = "error";
        err["code"]    = code;
        err["message"] = message;
        try {
            server_.send(hdl, err.dump(), websocketpp::frame::opcode::text);
        } catch (...) {}
    }

    // ── Broadcast timer ──────────────────────────────────────────────────

    void scheduleBroadcast()
    {
        auto timer = std::make_shared<asio::steady_timer>(
            server_.get_io_service(),
            std::chrono::milliseconds(100));

        timer->async_wait([this, timer](const std::error_code &ec) {
            if (ec) return;  // timer cancelled (server stopping)
            broadcastIfChanged();
            // Cheap to call every 100 ms — both functions exit immediately
            // if there's nothing to do.
            autoReportTick();
            autoReportWatchdog();
            scheduleBroadcast();  // reschedule
        });
    }

    void broadcastIfChanged()
    {
        if (connections_.empty()) return;

        // ── Fast VMon snapshot (high-frequency) ─────────────────────────
        auto [vmon_data, vmon_ver] = store_.getVMon();
        if (vmon_ver != last_vmon_ver_) {
            auto vmon_ts = store_.getVMonTs();
            std::string vmsg = R"({"type":"hv_vmon_snapshot","ts":)" +
                std::to_string(vmon_ts) + R"(,"data":)" + vmon_data + "}";
            auto hdls = connections_;
            for (auto &[hdl, info] : hdls) {
                try {
                    auto con = server_.get_con_from_hdl(hdl);
                    if (con->get_buffered_amount() > MAX_SEND_BUFFER) continue;
                    server_.send(hdl, vmsg, websocketpp::frame::opcode::text);
                } catch (...) {}
            }
            last_vmon_ver_ = vmon_ver;
        }

        // ── Full snapshots (lower-frequency) ────────────────────────────
        auto [hv_data,  hv_ver]  = store_.getHV();
        auto [bd_data,  bd_ver]  = store_.getBoard();
        auto [bst_data, bst_ver] = store_.getBooster();
        auto [cs_data,  cs_ver]  = store_.getCrateStatus();

        if (hv_ver != last_hv_ver_) {
            broadcast("hv_snapshot", hv_data);
            last_hv_ver_ = hv_ver;
        }
        if (bd_ver != last_bd_ver_) {
            broadcast("board_snapshot", bd_data);
            last_bd_ver_ = bd_ver;
        }
        if (bst_ver != last_bst_ver_) {
            broadcast("booster_snapshot", bst_data);
            last_bst_ver_ = bst_ver;
        }
        if (cs_ver != last_crate_status_ver_) {
            broadcast("crate_status", cs_data);
            last_crate_status_ver_ = cs_ver;
        }

        // ── Aggregation deltas (only when something flushed this tick) ──
        if (aggregator_) {
            std::string agg_delta = aggregator_->takeDeltaJson();
            if (agg_delta != "[]") {
                broadcast("hv_aggregation_update", agg_delta);
            }
        }

        // Fault logs: two separate buffers, two separate broadcasts
        uint64_t flf_cur = store_.faultLogFaultsVersion();
        if (flf_cur != last_flf_ver_) {
            auto [flf_data, flf_ver] = store_.getFaultLogFaultsSince(last_flf_ver_);
            broadcast("fault_log_faults", flf_data);
            last_flf_ver_ = flf_ver;
        }
        uint64_t flw_cur = store_.faultLogWarnsVersion();
        if (flw_cur != last_flw_ver_) {
            auto [flw_data, flw_ver] = store_.getFaultLogWarnsSince(last_flw_ver_);
            broadcast("fault_log_warns", flw_data);
            last_flw_ver_ = flw_ver;
        }

        // Settings save response: send back to the requesting client only
        if (store_.hasSettingsResponse()) {
            std::string resp = store_.takeSettingsResponse();
            std::string savedPath = store_.takeSettingsSavedPath();
            if (!resp.empty()) {
                std::lock_guard lk(settings_mu_);
                if (settings_pending_) {
                    settings_pending_ = false;
                    std::string msg = R"({"type":"settings_snapshot","data":)" + resp;
                    if (!savedPath.empty())
                        msg += R"(,"saved_path":")" + savedPath + R"(")";
                    msg += "}";
                    try {
                        server_.send(settings_requester_, msg,
                                     websocketpp::frame::opcode::text);
                    } catch (const std::exception &e) {
                        std::cerr << "Failed to send settings response: " << e.what() << "\n";
                    }
                }
            }
        }

        // Load settings response: send result back to requesting client
        if (store_.hasLoadResponse()) {
            std::string resp = store_.takeLoadResponse();
            if (!resp.empty()) {
                std::lock_guard lk(settings_mu_);
                if (load_pending_) {
                    load_pending_ = false;
                    std::string msg = R"({"type":"load_settings_done","data":)" + resp + "}";
                    try {
                        server_.send(load_requester_, msg,
                                     websocketpp::frame::opcode::text);
                    } catch (const std::exception &e) {
                        std::cerr << "Failed to send load response: " << e.what() << "\n";
                    }
                }
            }
        }
    }

    // Maximum bytes a single connection may have queued for sending.
    // Beyond this the client is considered too slow and is disconnected
    // to prevent unbounded memory growth.  With ~500 KB snapshots sent
    // every 2-3 s, 4 MB allows ~8 snapshots of headroom.
    static constexpr size_t MAX_SEND_BUFFER = 4 * 1024 * 1024;  // 4 MB

    void broadcast(const std::string &type, const std::string &data)
    {
        // Build envelope: {"type":"hv_snapshot","data":[...]}
        // We build the string manually to avoid re-parsing the already-
        // serialised JSON array from the pollers.
        std::string msg = R"({"type":")" + type + R"(","data":)" + data + "}";
        broadcastRaw(msg);
    }

    // Broadcast a JSON message verbatim — used for control-plane events
    // (auto_capture_done, …) whose payload is a flat object that already
    // owns its "type" field, instead of the type+data envelope above.
    void broadcastRaw(const std::string &msg)
    {
        // Copy handles — a failed send could trigger an async close that
        // modifies connections_ on the next event-loop tick.
        auto hdls = connections_;
        for (auto &[hdl, info] : hdls) {
            try {
                auto con = server_.get_con_from_hdl(hdl);
                if (con->get_buffered_amount() > MAX_SEND_BUFFER) {
                    std::cerr << fmt::format(
                        "Closing slow client (buffered {} bytes)\n",
                        con->get_buffered_amount());
                    server_.close(hdl, websocketpp::close::status::going_away,
                                  "send buffer overflow");
                    continue;
                }
                server_.send(hdl, msg, websocketpp::frame::opcode::text);
            } catch (const std::exception &e) {
                std::cerr << "Broadcast send error: " << e.what() << "\n";
            }
        }
    }

    void sendSnapshot(connection_hdl hdl,
                      const std::string &type,
                      const std::string &data)
    {
        std::string msg = R"({"type":")" + type + R"(","data":)" + data + "}";
        try {
            server_.send(hdl, msg, websocketpp::frame::opcode::text);
        } catch (...) {}
    }

    // ── HTTP static file server ─────────────────────────────────────────

    // ── HTTP REST API helpers ───────────────────────────────────────────

    // Authenticate an HTTP request via X-Auth header.
    // Returns the granted access level (0 = guest / unauthenticated).
    int httpAuthLevel(const std::string &pw) const
    {
        if (!authRequired()) return 2;       // no passwords → full access
        if (!expert_pass_.empty() && pw == expert_pass_) return 2;
        if (!user_pass_.empty()   && pw == user_pass_)   return 1;
        return 0;
    }

    void httpJson(connection_hdl hdl, websocketpp::http::status_code::value code,
                  const json &body)
    {
        try {
            auto con = server_.get_con_from_hdl(hdl);
            con->set_status(code);
            con->append_header("Content-Type", "application/json; charset=utf-8");
            con->append_header("Access-Control-Allow-Origin", "*");
            con->set_body(body.dump());
        } catch (...) {}
    }

    void onHttp(connection_hdl hdl)
    {
        auto con = server_.get_con_from_hdl(hdl);
        std::string uri = con->get_resource();  // e.g. "/", "/monitor.js"
        std::string method = con->get_request().get_method();

        // ── REST API endpoints (/api/...) ───────────────────────────────
        if (uri.rfind("/api/", 0) == 0) {
            // CORS preflight
            if (method == "OPTIONS") {
                con->set_status(websocketpp::http::status_code::ok);
                con->append_header("Access-Control-Allow-Origin", "*");
                con->append_header("Access-Control-Allow-Headers",
                                   "Content-Type, X-Auth");
                con->append_header("Access-Control-Allow-Methods",
                                   "GET, POST, OPTIONS");
                return;
            }

            std::string pw = con->get_request_header("X-Auth");
            int level = httpAuthLevel(pw);

            // GET /api/voltage?name=W1124
            if (uri.rfind("/api/voltage", 0) == 0 && method == "GET") {
                // Parse ?name= from query string
                std::string name;
                auto qpos = uri.find('?');
                if (qpos != std::string::npos) {
                    std::string qs = uri.substr(qpos + 1);
                    // Simple single-param parse: name=VALUE
                    auto eqpos = qs.find('=');
                    if (eqpos != std::string::npos &&
                        qs.substr(0, eqpos) == "name")
                        name = qs.substr(eqpos + 1);
                }
                if (name.empty()) {
                    httpJson(hdl, websocketpp::http::status_code::bad_request,
                             {{"error", "missing 'name' query parameter"}});
                    return;
                }
                auto [snap, ver] = store_.getHV();
                json channels = json::parse(snap, nullptr, false);
                if (!channels.is_array()) {
                    httpJson(hdl, websocketpp::http::status_code::service_unavailable,
                             {{"error", "no HV data available yet"}});
                    return;
                }
                for (const auto &ch : channels) {
                    if (ch.value("name", "") == name) {
                        json resp;
                        for (const char *key : {"name","crate","slot","channel",
                                                "vset","vmon","limit","iset","imon",
                                                "on","status"}) {
                            if (ch.contains(key)) resp[key] = ch[key];
                        }
                        httpJson(hdl, websocketpp::http::status_code::ok, resp);
                        return;
                    }
                }
                httpJson(hdl, websocketpp::http::status_code::not_found,
                         {{"error", "channel '" + name + "' not found"}});
                return;
            }

            // POST /api/voltage  { "name": "W1124", "value": 1500.0 }
            if (uri == "/api/voltage" && method == "POST") {
                if (level < 2) {
                    httpJson(hdl, websocketpp::http::status_code::forbidden,
                             {{"error", "Expert access required (set X-Auth header)"}});
                    return;
                }
                json body;
                try { body = json::parse(con->get_request_body()); }
                catch (...) {
                    httpJson(hdl, websocketpp::http::status_code::bad_request,
                             {{"error", "invalid JSON body"}});
                    return;
                }
                std::string name = body.value("name", "");
                if (name.empty() || !body.contains("value") ||
                    !body["value"].is_number()) {
                    httpJson(hdl, websocketpp::http::status_code::bad_request,
                             {{"error", "requires 'name' (string) and 'value' (number)"}});
                    return;
                }
                json cmd;
                cmd["type"]  = "set_voltage_by_name";
                cmd["name"]  = name;
                cmd["value"] = body["value"];
                if (op_logger_) op_logger_->logCommand(level, cmd);
                cmdq_.push({ Command::HV, std::move(cmd) });
                httpJson(hdl, websocketpp::http::status_code::ok,
                         {{"status", "queued"},
                          {"name", name}});
                return;
            }

            // POST /api/auth  { "password": "..." }
            if (uri == "/api/auth" && method == "POST") {
                json body;
                try { body = json::parse(con->get_request_body()); }
                catch (...) {
                    httpJson(hdl, websocketpp::http::status_code::bad_request,
                             {{"error", "invalid JSON body"}});
                    return;
                }
                std::string testPw = body.value("password", "");
                int granted = httpAuthLevel(testPw);
                httpJson(hdl, websocketpp::http::status_code::ok,
                         {{"granted", granted}});
                return;
            }

            // POST /api/elog/post  — chosen reporter uploads its capture.
            // Body: { "xml": "<elog xml>", "auto": true, "request_id": "…" }
            // Always archives <save_dir>/<UTC date>/report_<UTC>.xml +
            // decoded PNG attachments; if post_to_elog is true, then also
            // shells out to curl --upload-file with the configured cert.
            if (uri == "/api/elog/post" && method == "POST") {
                json result = handleElogPost(con->get_request_body());
                bool ok = result.value("ok", false);
                httpJson(hdl, ok ? websocketpp::http::status_code::ok
                                 : websocketpp::http::status_code::bad_request,
                         result);
                return;
            }

            httpJson(hdl, websocketpp::http::status_code::not_found,
                     {{"error", "unknown API endpoint"}});
            return;
        }

        // ── Static file serving ─────────────────────────────────────────

        // Redirect "/" to "/monitor.html"
        if (uri == "/") uri = "/monitor.html";

        // Security: reject paths with ".." to prevent directory traversal
        if (uri.find("..") != std::string::npos) {
            con->set_status(websocketpp::http::status_code::forbidden);
            con->set_body("403 Forbidden");
            return;
        }

        // If no resource dir configured, return a helpful message
        if (resource_dir_.empty()) {
            con->set_status(websocketpp::http::status_code::not_found);
            con->set_body("No resource directory configured. "
                          "Use -r <path> to serve the dashboard.");
            return;
        }

        // Build filesystem path. Reject anything that isn't a regular
        // file: opening a directory succeeds on Linux, but the first
        // read throws ios_base::failure ("Is a directory") which would
        // propagate out of the handler and terminate the daemon.
        std::string filepath = resource_dir_ + uri;
        std::error_code ec;
        if (!std::filesystem::is_regular_file(filepath, ec)) {
            con->set_status(websocketpp::http::status_code::not_found);
            con->set_body("404 Not Found: " + uri);
            return;
        }

        std::string body;
        try {
            std::ifstream f(filepath, std::ios::binary);
            if (!f.is_open()) {
                con->set_status(websocketpp::http::status_code::not_found);
                con->set_body("404 Not Found: " + uri);
                return;
            }
            body.assign(std::istreambuf_iterator<char>(f),
                        std::istreambuf_iterator<char>());
        } catch (const std::exception &e) {
            std::cerr << "onHttp: read failed for " << filepath
                      << ": " << e.what() << "\n";
            con->set_status(websocketpp::http::status_code::internal_server_error);
            con->set_body("500 Internal Server Error");
            return;
        }

        con->set_status(websocketpp::http::status_code::ok);
        con->append_header("Content-Type", mimeType(uri));
        con->append_header("Cache-Control", "no-cache");
        con->set_body(body);
    }

    static std::string mimeType(const std::string &path)
    {
        auto ext = path.substr(path.rfind('.') + 1);
        if (ext == "html") return "text/html; charset=utf-8";
        if (ext == "js")   return "application/javascript; charset=utf-8";
        if (ext == "css")  return "text/css; charset=utf-8";
        if (ext == "json") return "application/json; charset=utf-8";
        if (ext == "png")  return "image/png";
        if (ext == "svg")  return "image/svg+xml";
        if (ext == "ico")  return "image/x-icon";
        return "application/octet-stream";
    }

    // ─── Auto-report dispatcher ──────────────────────────────────────────

    // Probe local_save_dir.  Disable post-to-elog cleanly if the dir
    // can't be created or written to — we mirror eviewer's "save first,
    // upload second" discipline so we never push an XML to elog without
    // having the same bytes archived locally for later replay.
    void checkSaveDirWritable()
    {
        save_dir_writable_ = false;
        if (auto_report_cfg_.local_save_dir.empty()) {
            std::cerr << "AutoReport: local_save_dir not configured — "
                         "/api/elog/post will refuse posts\n";
            return;
        }
        std::filesystem::path dir = auto_report_cfg_.local_save_dir;
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (ec) {
            std::cerr << "AutoReport: cannot create " << dir
                      << " (" << ec.message() << ") — disabled\n";
            return;
        }
        std::filesystem::path probe = dir / ".prad2hv_write_check";
        {
            std::ofstream f(probe);
            if (!f) {
                std::cerr << "AutoReport: " << dir
                          << " is not writable — disabled\n";
                return;
            }
            f << "ok\n";
        }
        std::filesystem::remove(probe, ec);
        save_dir_writable_ = true;
        std::cerr << "AutoReport: save_dir " << dir << " is writable\n";
    }

    // Sanitised view of the auto_report config that the browser receives
    // on /init.  Cert/key paths are intentionally *not* exposed; they
    // belong on the daemon host only and are read by the curl child
    // process when post_to_elog fires.
    json autoReportClientView() const
    {
        return {
            {"enabled",              auto_report_cfg_.enabled},
            {"post_to_elog",         auto_report_cfg_.post_to_elog},
            {"min_interval_ms",      auto_report_cfg_.min_interval_ms},
            {"scheduled_interval_s", auto_report_cfg_.scheduled_interval_s},
            {"elog", {
                {"url",     auto_report_cfg_.elog_url},
                {"logbook", auto_report_cfg_.elog_logbook},
                {"author",  auto_report_cfg_.elog_author},
                {"tags",    auto_report_cfg_.elog_tags},
            }},
        };
    }

    // ── Time helpers ─────────────────────────────────────────────────
    // ISO-8601 Z, used in summary.json records.
    static std::string isoNowZ()
    {
        std::time_t t = std::time(nullptr);
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&t));
        return buf;
    }
    // Today's UTC date — bucket key for archive layout.
    static std::string utcDate()
    {
        std::time_t t = std::time(nullptr);
        char buf[16];
        std::strftime(buf, sizeof(buf), "%Y-%m-%d", std::gmtime(&t));
        return buf;
    }
    // Compact UTC stamp, used in archived XML basenames + upload names.
    static std::string utcStamp()
    {
        std::time_t t = std::time(nullptr);
        char buf[24];
        std::strftime(buf, sizeof(buf), "%Y%m%dT%H%M%SZ", std::gmtime(&t));
        return buf;
    }

    // ── Base64 decode for <Attachment><data encoding="base64"> blocks ──
    // Streaming variant — strips whitespace, ignores anything outside the
    // standard alphabet.  Mirrors viewer_server_http.cpp:_b64Decode so an
    // archived prad2hv XML opens with the same tooling as a prad2 XML.
    static std::string b64Decode(const std::string &s)
    {
        static const int8_t TBL[256] = {
            -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
            -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
            52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,-1, 0, 1, 2, 3, 4, 5, 6,
             7, 8, 9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
            -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,
            49,50,51,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
            -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
            -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
            -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
            -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
            -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
        };
        std::string out;
        out.reserve(s.size() * 3 / 4);
        int val = 0, valb = -8;
        for (unsigned char c : s) {
            if (TBL[c] < 0) continue;
            val = (val << 6) | TBL[c];
            valb += 6;
            if (valb >= 0) {
                out.push_back(static_cast<char>((val >> valb) & 0xFF));
                valb -= 8;
            }
        }
        return out;
    }

    // Walk <Attachment> blocks in the elog XML, decode each base64 body,
    // and write the bytes to <dir>/<filename>.  filename is sanitised
    // (path components stripped) so a hostile client can't escape the
    // bucket directory.
    static void extractXmlAttachments(const std::string &xml,
                                      const std::filesystem::path &dir)
    {
        static const std::string A_OPEN  = "<Attachment>";
        static const std::string A_CLOSE = "</Attachment>";
        static const std::string F_OPEN  = "<filename>";
        static const std::string F_CLOSE = "</filename>";
        static const std::string D_OPEN  = "<data encoding=\"base64\">";
        static const std::string D_CLOSE = "</data>";
        size_t pos = 0;
        while (true) {
            auto a_beg = xml.find(A_OPEN, pos);
            if (a_beg == std::string::npos) break;
            auto a_end = xml.find(A_CLOSE, a_beg);
            if (a_end == std::string::npos) break;
            const std::string block = xml.substr(a_beg, a_end - a_beg);
            pos = a_end + A_CLOSE.size();

            auto fb = block.find(F_OPEN);
            auto fe = block.find(F_CLOSE);
            auto db = block.find(D_OPEN);
            auto de = block.find(D_CLOSE);
            if (fb == std::string::npos || fe == std::string::npos ||
                db == std::string::npos || de == std::string::npos) continue;
            std::string filename = block.substr(fb + F_OPEN.size(),
                                                fe - fb - F_OPEN.size());
            std::string b64      = block.substr(db + D_OPEN.size(),
                                                de - db - D_OPEN.size());
            auto slash = filename.find_last_of("/\\");
            if (slash != std::string::npos) filename = filename.substr(slash + 1);
            if (filename.empty()) continue;

            std::ofstream f(dir / filename, std::ios::binary);
            if (!f) {
                std::cerr << "AutoReport: open " << (dir / filename)
                          << " failed\n";
                continue;
            }
            std::string raw = b64Decode(b64);
            f.write(raw.data(), static_cast<std::streamsize>(raw.size()));
        }
    }

    struct LocalSaveResult {
        std::filesystem::path dir;
        std::filesystem::path xml;
        bool        ok = false;
        std::string error;
    };

    LocalSaveResult saveReportLocally(const std::string &xml_body)
    {
        LocalSaveResult r;
        if (auto_report_cfg_.local_save_dir.empty()) {
            r.error = "local_save_dir not configured";
            return r;
        }
        r.dir = std::filesystem::path(auto_report_cfg_.local_save_dir) / utcDate();
        std::error_code ec;
        std::filesystem::create_directories(r.dir, ec);
        if (ec) {
            r.error = "mkdir " + r.dir.string() + ": " + ec.message();
            return r;
        }
        r.xml = r.dir / (std::string("report_") + utcStamp() + ".xml");
        {
            std::ofstream f(r.xml);
            if (!f) { r.error = "open " + r.xml.string() + " failed"; return r; }
            f << xml_body;
            if (!f.good()) {
                r.error = "write " + r.xml.string() + " failed";
                return r;
            }
        }
        extractXmlAttachments(xml_body, r.dir);
        r.ok = true;
        return r;
    }

    // Append a one-line summary record per attempt — same shape eviewer
    // writes, so existing replay tooling that diffs success/fail across
    // both daemons works unchanged.
    void appendAutoReportSummary(const std::string &saved_xml,
                                 bool posted,
                                 const std::string &lognumber,
                                 const std::string &reason,
                                 const std::string &error)
    {
        if (auto_report_cfg_.local_save_dir.empty()) return;
        std::filesystem::path path =
            std::filesystem::path(auto_report_cfg_.local_save_dir) / "summary.json";
        json doc = json::object();
        {
            std::ifstream f(path);
            if (f) {
                std::stringstream ss; ss << f.rdbuf();
                auto parsed = json::parse(ss.str(), nullptr, false);
                if (!parsed.is_discarded() && parsed.is_object()) doc = parsed;
            }
        }
        doc["updated"]            = isoNowZ();
        doc["auto_post_enabled"]  = auto_report_cfg_.enabled;
        doc["post_to_elog"]       = auto_report_cfg_.post_to_elog;
        doc["save_dir_writable"]  = save_dir_writable_;
        doc["min_interval_ms"]    = auto_report_cfg_.min_interval_ms;
        if (!doc.contains("entries") || !doc["entries"].is_array())
            doc["entries"] = json::array();

        json rec = {
            {"saved_xml", saved_xml},
            {"saved_at",  isoNowZ()},
            {"posted",    posted},
            {"lognumber", lognumber},
            {"reason",    reason},
        };
        if (!error.empty()) rec["error"] = error;
        doc["entries"].push_back(rec);

        std::ofstream f(path, std::ios::trunc);
        if (!f) {
            std::cerr << "AutoReport summary: cannot write " << path << "\n";
            return;
        }
        f << doc.dump(2) << "\n";
    }

    // Send capture_request to one reporter-capable client that hasn't
    // already failed for this in-flight capture.  Returns false if no
    // such client exists (caller decides whether to log + clear pending).
    // Mirrors viewer_server_http.cpp:dispatchCapture, minus the run
    // number plumbing — HV is event-less.
    static constexpr int CAPTURE_TIMEOUT_S = 30;

    bool dispatchCapture(const std::string &reason,
                         const std::string &request_id_in)
    {
        if (!auto_report_cfg_.enabled) return false;
        if (!save_dir_writable_) {
            std::cerr << "AutoReport: dispatch skipped (save_dir not writable)\n";
            appendAutoReportSummary("", false, "", reason,
                                    "save_dir not writable");
            return false;
        }
        if (reporter_capable_.empty()) {
            std::string detail = connections_.empty()
                ? "no connected client"
                : "no reporter-capable client (" +
                  std::to_string(connections_.size()) +
                  " legacy tab(s) connected — refresh required)";
            std::cerr << "AutoReport: " << detail
                      << " (" << reason << ")\n";
            appendAutoReportSummary("", false, "", reason, detail);
            return false;
        }

        std::time_t now_t = std::time(nullptr);
        PendingCapture pc;
        if (!request_id_in.empty() && pending_capture_ &&
            pending_capture_->request_id == request_id_in) {
            // Watchdog retry — keep the same request_id, advance to next.
            pc = *pending_capture_;
        } else if (pending_capture_ &&
                   (now_t - pending_capture_->started) < CAPTURE_TIMEOUT_S) {
            std::cerr << "AutoReport: dispatch skipped — capture (request "
                      << pending_capture_->request_id << ") still in flight\n";
            return false;
        } else {
            char buf[40];
            std::snprintf(buf, sizeof(buf), "%lx-%u",
                          static_cast<long>(now_t), ++req_counter_);
            pc.request_id = buf;
            pc.reason     = reason;
            pc.tried.clear();
        }
        pc.started = now_t;

        connection_hdl chosen;
        bool found = false;
        for (const auto &h : reporter_capable_) {
            if (pc.tried.find(h) == pc.tried.end()) {
                chosen = h; found = true; break;
            }
        }
        if (!found) {
            std::cerr << "AutoReport: all reporters exhausted (request "
                      << pc.request_id << ")\n";
            appendAutoReportSummary("", false, "", reason,
                                    "all clients timed out");
            pending_capture_.reset();
            return false;
        }
        pc.tried.insert(chosen);
        pending_capture_ = pc;

        json msg = {
            {"type",       "capture_request"},
            {"request_id", pc.request_id},
            {"reason",     pc.reason},
        };
        try {
            server_.send(chosen, msg.dump(),
                         websocketpp::frame::opcode::text);
        } catch (const std::exception &e) {
            std::cerr << "AutoReport: send failed (" << e.what()
                      << "), letting watchdog retry\n";
            return false;
        }
        std::cerr << "AutoReport: dispatched capture_request (request "
                  << pc.request_id << ", attempt " << pc.tried.size()
                  << ", reason=" << pc.reason << ")\n";
        return true;
    }

    // 100-ms timer hook — re-dispatch a stuck capture to the next
    // reporter-capable client.
    void autoReportWatchdog()
    {
        if (!pending_capture_) return;
        if (std::time(nullptr) - pending_capture_->started <= CAPTURE_TIMEOUT_S)
            return;
        auto stale = *pending_capture_;
        std::cerr << "AutoReport: watchdog timeout (request "
                  << stale.request_id << ")\n";
        dispatchCapture(stale.reason, stale.request_id);
    }

    // 100-ms timer hook — fire a scheduled capture if the configured
    // interval has elapsed since the last attempt.  We update
    // last_scheduled_fire_ even on dispatch failures so a misconfigured
    // run (no reporters, save_dir broken, …) doesn't loop-fire every
    // tick.
    void autoReportTick()
    {
        if (!auto_report_cfg_.enabled) return;
        if (auto_report_cfg_.scheduled_interval_s <= 0) return;
        std::time_t now_t = std::time(nullptr);
        if (last_scheduled_fire_ == 0) {
            last_scheduled_fire_ = now_t;
            return;  // skip the first tick — daemon just started
        }
        if (now_t - last_scheduled_fire_ < auto_report_cfg_.scheduled_interval_s)
            return;
        last_scheduled_fire_ = now_t;
        dispatchCapture("scheduled", "");
    }

    // POST /api/elog/post — body is JSON {xml, auto, request_id}.
    // Always writes to <save_dir>/<UTC date>/report_<UTC>.xml first, then
    // optionally shells out to curl for the upload.  Same dedup +
    // local-save discipline as viewer_server_http.cpp:handleElogPost,
    // but date-bucketed (no run number) and minus the ws_clients_ split.
    json handleElogPost(const std::string &body)
    {
        if (body.empty())
            return {{"ok", false}, {"error", "empty body"}};
        if (auto_report_cfg_.elog_url.empty())
            return {{"ok", false}, {"error", "elog url not configured"}};

        auto req = json::parse(body, nullptr, false);
        if (req.is_discarded() || !req.is_object() || !req.contains("xml"))
            return {{"ok", false}, {"error", "invalid request"}};

        std::string xml_body  = req["xml"].get<std::string>();
        bool        is_auto   = req.value("auto", false);
        std::string request_id = req.value("request_id", std::string());

        // auto:true must reference the current pending request — guards
        // against a buggy / replayed POST clobbering an in-flight capture.
        if (is_auto) {
            if (request_id.empty())
                return {{"ok", false},
                        {"error", "auto post requires request_id"}};
            if (!pending_capture_ ||
                pending_capture_->request_id != request_id)
                return {{"ok", false}, {"error", "stale request_id"}};
        }

        if (auto_report_cfg_.local_save_dir.empty())
            return {{"ok", false},
                    {"error", "auto_report.local_save_dir is required"}};
        if (!save_dir_writable_)
            return {{"ok", false},
                    {"error", "local_save_dir not writable"}};

        std::unique_lock<std::mutex> post_lock(elog_post_mtx_);

        // Server-side dedup.  Skip if today's bucket already holds an XML
        // newer than min_interval_ms ago.  Catches scheduled-tick + watchdog
        // retry races.
        if (auto_report_cfg_.min_interval_ms > 0) {
            std::filesystem::path bucket =
                std::filesystem::path(auto_report_cfg_.local_save_dir) / utcDate();
            std::error_code ec;
            if (std::filesystem::exists(bucket, ec) &&
                std::filesystem::is_directory(bucket, ec))
            {
                std::time_t now_t = std::time(nullptr);
                int window_sec = auto_report_cfg_.min_interval_ms / 1000;
                std::filesystem::path most_recent;
                std::time_t most_recent_t = 0;
                for (auto &e : std::filesystem::directory_iterator(bucket, ec)) {
                    if (!e.is_regular_file()) continue;
                    if (e.path().extension() != ".xml") continue;
                    struct stat st;
                    if (::stat(e.path().c_str(), &st) != 0) continue;
                    if (st.st_mtime > most_recent_t) {
                        most_recent_t = st.st_mtime;
                        most_recent   = e.path();
                    }
                }
                if (most_recent_t && (now_t - most_recent_t) < window_sec) {
                    std::cerr << "AutoReport: server-side dup skip ("
                              << most_recent << ", "
                              << (now_t - most_recent_t) << "s old)\n";
                    return {
                        {"ok", true}, {"skipped", true},
                        {"detail", "server-side dedup: recent save within "
                                   + std::to_string(window_sec / 60) + " min"},
                        {"saved_dir", bucket.string()},
                        {"saved_xml", most_recent.string()}};
                }
            }
        }

        auto sr = saveReportLocally(xml_body);
        if (!sr.ok) {
            std::cerr << "AutoReport: local-save failed: " << sr.error << "\n";
            return {{"ok", false}, {"error", "local save failed: " + sr.error}};
        }
        const std::string saved_dir = sr.dir.string();
        const std::string saved_xml = sr.xml.string();

        // Dry-run: archive only, no upload.
        if (is_auto && !auto_report_cfg_.post_to_elog) {
            std::cerr << "AutoReport: auto-mode dry run (post_to_elog=false)"
                      << " saved=" << saved_xml << "\n";
            // Capture the reason BEFORE clearing pending_capture_ —
            // appendAutoReportSummary needs it for the audit record.
            std::string reason_for_log = pending_capture_
                ? pending_capture_->reason : std::string("manual");
            // Clear the pending slot + notify all clients before
            // returning so the badge drops cleanly even on dry runs.
            if (is_auto && !request_id.empty() && pending_capture_ &&
                pending_capture_->request_id == request_id)
            {
                pending_capture_.reset();
                json done = {{"type", "auto_capture_done"},
                             {"posted", false}, {"dry_run", true},
                             {"saved_xml", saved_xml}};
                broadcastRaw(done.dump());
            }
            appendAutoReportSummary(saved_xml, false, "", reason_for_log, "");
            return {{"ok", true}, {"posted", false}, {"dry_run", true},
                    {"saved_dir", saved_dir}, {"saved_xml", saved_xml},
                    {"status", "dry_run"}};
        }

        post_lock.unlock();

        // Upload via curl, mirroring eviewer's command.  /incoming/<name>
        // is a one-shot key on the elog side, so the timestamped basename
        // doubles as a per-attempt uniqueness guard.
        std::string cert_flag;
        if (!auto_report_cfg_.elog_cert.empty())
            cert_flag = " --cert '" + auto_report_cfg_.elog_cert
                      + "' --key '"  + auto_report_cfg_.elog_key + "'";
        std::string upload_name = "prad2hv_" + utcStamp() + ".xml";
        std::string cmd = "curl -s -o /dev/null -w '%{http_code}'" + cert_flag
                        + " --upload-file '" + saved_xml + "' '"
                        + auto_report_cfg_.elog_url + "/incoming/" + upload_name
                        + "' 2>/dev/null";
        std::string http_code;
        FILE *p = popen(cmd.c_str(), "r");
        if (p) {
            char buf[256] = {};
            if (fgets(buf, sizeof(buf), p)) http_code = buf;
            while (!http_code.empty() &&
                   (http_code.back() == '\n' || http_code.back() == '\r'))
                http_code.pop_back();
            pclose(p);
        }
        bool ok = (http_code.find("200") != std::string::npos ||
                   http_code.find("201") != std::string::npos);
        std::cerr << "AutoReport: " << auto_report_cfg_.elog_url << " <- "
                  << saved_xml << " -> HTTP " << http_code
                  << (ok ? " OK" : " FAIL") << "\n";

        std::string reason_for_log = is_auto && pending_capture_
            ? pending_capture_->reason : std::string("manual");

        if (is_auto && !request_id.empty() && pending_capture_ &&
            pending_capture_->request_id == request_id)
        {
            pending_capture_.reset();
            json done = {{"type", "auto_capture_done"},
                         {"posted", ok},
                         {"saved_xml", saved_xml}};
            broadcast("auto_capture_done", done.dump());
        }
        appendAutoReportSummary(saved_xml, ok, "", reason_for_log,
                                ok ? std::string()
                                   : "upload http " + http_code);
        return {{"ok", ok}, {"posted", ok}, {"status", http_code},
                {"saved_dir", saved_dir}, {"saved_xml", saved_xml}};
    }

    // ── Helpers ──────────────────────────────────────────────────────────

    static std::string readFile(const std::string &path,
                                const char *fallback)
    {
        if (path.empty()) return fallback;
        std::error_code ec;
        if (!std::filesystem::is_regular_file(path, ec)) {
            std::cerr << "Cannot open file (not a regular file): "
                      << path << "\n";
            return fallback;
        }
        try {
            std::ifstream f(path);
            if (!f.is_open()) {
                std::cerr << "Cannot open file: " << path << "\n";
                return fallback;
            }
            return std::string(std::istreambuf_iterator<char>(f),
                               std::istreambuf_iterator<char>());
        } catch (const std::exception &e) {
            std::cerr << "Read failed for " << path << ": "
                      << e.what() << "\n";
            return fallback;
        }
    }

    // ── Data ─────────────────────────────────────────────────────────────
    uint16_t       port_;
    SnapshotStore &store_;
    CommandQueue  &cmdq_;
    std::string    resource_dir_;

    WsServerType   server_;

    // Owner-based comparison for weak_ptr handles
    struct hdl_less {
        bool operator()(connection_hdl a, connection_hdl b) const {
            return a.owner_before(b);
        }
    };
    std::map<connection_hdl, ClientInfo, hdl_less> connections_;

    // Version tracking for change-detection broadcasts
    uint64_t last_vmon_ver_ = 0;  // fast VMon-only
    uint64_t last_hv_ver_  = 0;
    uint64_t last_bd_ver_  = 0;
    uint64_t last_bst_ver_ = 0;
    uint64_t last_flf_ver_ = 0;   // fault log — faults
    uint64_t last_flw_ver_ = 0;   // fault log — warnings
    uint64_t last_crate_status_ver_ = 0;  // crate connection status

    // Settings save/load request-response tracking
    std::mutex settings_mu_;
    connection_hdl settings_requester_;
    bool settings_pending_ = false;
    connection_hdl load_requester_;
    bool load_pending_ = false;

    // Pre-loaded static config (sent to each client on connect)
    std::string module_geo_json_;
    std::string gui_config_json_;

    // Access control passwords (empty = not configured)
    std::string user_pass_;
    std::string expert_pass_;

    // Operation logger (optional — nullptr disables)
    FileOpLogger *op_logger_ = nullptr;
    // Aggregator (optional — nullptr → no hv_aggregation init/deltas)
    HVAggregator *aggregator_ = nullptr;

    // ── Auto-report state ───────────────────────────────────────────────
    AutoReportConfig auto_report_cfg_;
    bool             save_dir_writable_ = false;
    // Reporter-capable subset of connections_.  Populated when a client
    // sends client_hello with capabilities:["auto_report"]; pruned in
    // onClose.  The asio io_context single-threads all connection events
    // so no extra mutex is needed beyond the implicit thread safety.
    std::set<connection_hdl, hdl_less> reporter_capable_;

    struct PendingCapture {
        std::string request_id;
        std::string reason;
        std::time_t started = 0;     // unix seconds, last dispatch attempt
        std::set<connection_hdl, hdl_less> tried;
    };
    std::optional<PendingCapture> pending_capture_;
    std::time_t                   last_scheduled_fire_ = 0;
    unsigned                      req_counter_         = 0;
    // Guards the dedup-check + local-save sequence inside
    // /api/elog/post.  Even though websocketpp serialises HTTP handlers
    // on the asio thread, holding a lock keeps the discipline explicit
    // and matches eviewer's invariant (and lets us drop into a worker
    // thread later without re-thinking the race window).
    std::mutex                    elog_post_mtx_;
};
