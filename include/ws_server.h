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
#include <string>
#include <iostream>
#include <fstream>
#include <functional>
#include <chrono>

using json = nlohmann::json;

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
             const std::string &daq_map_path    = "",
             const std::string &user_password   = "",
             const std::string &expert_password = "",
             FileOpLogger      *op_logger       = nullptr)
        : port_(port), store_(store), cmdq_(cmdq),
          resource_dir_(resource_dir),
          user_pass_(user_password),
          expert_pass_(expert_password),
          op_logger_(op_logger)
    {
        // Pre-load static config files into memory
        module_geo_json_ = readFile(module_geo_path, "[]");
        gui_config_json_ = readFile(gui_config_path, "{}");
        daq_map_json_    = readFile(daq_map_path,    "[]");

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
        init["daq_map"]            = json::parse(daq_map_json_,    nullptr, false);
        init["fault_log_capacity"] = store_.faultLogCapacity();
        init["auth_required"]      = authRequired();
        init["access_level"]       = info.accessLevel;

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
        std::cout << fmt::format("Client disconnected ({} remaining)\n",
                                 connections_.size());
    }

    void onMessage(connection_hdl hdl, WsServerType::message_ptr msg)
    {
        try {
            json cmd = json::parse(msg->get_payload());
            std::string type = cmd.value("type", "");

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

        // Build filesystem path
        std::string filepath = resource_dir_ + uri;
        std::ifstream f(filepath, std::ios::binary);
        if (!f.is_open()) {
            con->set_status(websocketpp::http::status_code::not_found);
            con->set_body("404 Not Found: " + uri);
            return;
        }

        std::string body((std::istreambuf_iterator<char>(f)),
                          std::istreambuf_iterator<char>());

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

    // ── Helpers ──────────────────────────────────────────────────────────

    static std::string readFile(const std::string &path,
                                const char *fallback)
    {
        if (path.empty()) return fallback;
        std::ifstream f(path);
        if (!f.is_open()) {
            std::cerr << "Cannot open file: " << path << "\n";
            return fallback;
        }
        return std::string((std::istreambuf_iterator<char>(f)),
                            std::istreambuf_iterator<char>());
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
    std::string daq_map_json_;

    // Access control passwords (empty = not configured)
    std::string user_pass_;
    std::string expert_pass_;

    // Operation logger (optional — nullptr disables)
    FileOpLogger *op_logger_ = nullptr;
};
