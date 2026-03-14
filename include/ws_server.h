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
// ─────────────────────────────────────────────────────────────────────────────

#ifndef ASIO_STANDALONE
#define ASIO_STANDALONE
#endif

#include <websocketpp/config/asio_no_tls.hpp>
#include <websocketpp/server.hpp>
#include <nlohmann/json.hpp>

#include "hv_daemon.h"

#include <set>
#include <string>
#include <iostream>
#include <fstream>
#include <functional>
#include <chrono>

using json = nlohmann::json;
using WsServerType = websocketpp::server<websocketpp::config::asio>;
using connection_hdl = websocketpp::connection_hdl;


class WsServer
{
public:
    WsServer(uint16_t port,
             SnapshotStore &store,
             CommandQueue  &cmdq,
             const std::string &module_geo_path = "",
             const std::string &gui_config_path = "",
             const std::string &daq_map_path    = "")
        : port_(port), store_(store), cmdq_(cmdq)
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
    }

    // Blocks on the Asio event loop.  Call stop() from a signal handler
    // or another thread to break out.
    void run()
    {
        server_.listen(port_);
        server_.start_accept();

        std::cout << fmt::format("WebSocket server listening on port {}\n", port_);

        // Set up a recurring timer to broadcast snapshots
        scheduleBroadcast();

        server_.run();
    }

    void stop()
    {
        server_.stop_listening();
        // Close all connections gracefully
        for (auto &hdl : connections_) {
            try {
                server_.close(hdl, websocketpp::close::status::going_away,
                              "daemon shutting down");
            } catch (...) {}
        }
        server_.stop();
    }

    size_t clientCount() const { return connections_.size(); }

private:
    // ── Connection lifecycle ─────────────────────────────────────────────

    void onOpen(connection_hdl hdl)
    {
        connections_.insert(hdl);
        std::cout << fmt::format("Client connected ({} total)\n",
                                 connections_.size());

        // Send static config to the new client immediately
        json init;
        init["type"]           = "init";
        init["module_geometry"] = json::parse(module_geo_json_, nullptr, false);
        init["gui_config"]     = json::parse(gui_config_json_, nullptr, false);
        init["daq_map"]        = json::parse(daq_map_json_,    nullptr, false);

        try {
            server_.send(hdl, init.dump(), websocketpp::frame::opcode::text);
        } catch (const std::exception &e) {
            std::cerr << "Failed to send init to client: " << e.what() << "\n";
        }

        // Also send current snapshots right away
        sendSnapshot(hdl, "hv_snapshot",      store_.getHV().first);
        sendSnapshot(hdl, "board_snapshot",   store_.getBoard().first);
        sendSnapshot(hdl, "booster_snapshot", store_.getBooster().first);
    }

    void onClose(connection_hdl hdl)
    {
        connections_.erase(hdl);
        std::cout << fmt::format("Client disconnected ({} remaining)\n",
                                 connections_.size());
    }

    void onMessage(connection_hdl /*hdl*/, WsServerType::message_ptr msg)
    {
        try {
            json cmd = json::parse(msg->get_payload());
            std::string type = cmd.value("type", "");

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

    // ── Broadcast timer ──────────────────────────────────────────────────

    void scheduleBroadcast()
    {
        auto timer = std::make_shared<asio::steady_timer>(
            server_.get_io_service(),
            std::chrono::milliseconds(200));

        timer->async_wait([this, timer](const std::error_code &ec) {
            if (ec) return;  // timer cancelled (server stopping)
            broadcastIfChanged();
            scheduleBroadcast();  // reschedule
        });
    }

    void broadcastIfChanged()
    {
        if (connections_.empty()) return;

        auto [hv_data,  hv_ver]  = store_.getHV();
        auto [bd_data,  bd_ver]  = store_.getBoard();
        auto [bst_data, bst_ver] = store_.getBooster();

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
    }

    void broadcast(const std::string &type, const std::string &data)
    {
        // Build envelope: {"type":"hv_snapshot","data":[...]}
        // We build the string manually to avoid re-parsing the already-
        // serialised JSON array from the pollers.
        std::string msg = R"({"type":")" + type + R"(","data":)" + data + "}";

        // Copy handles — a failed send could trigger an async close that
        // modifies connections_ on the next event-loop tick.
        auto hdls = connections_;
        for (auto &hdl : hdls) {
            try {
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

    WsServerType   server_;

    // Owner-based comparison for weak_ptr handles
    struct hdl_less {
        bool operator()(connection_hdl a, connection_hdl b) const {
            return a.owner_before(b);
        }
    };
    std::set<connection_hdl, hdl_less> connections_;

    // Version tracking for change-detection broadcasts
    uint64_t last_hv_ver_  = 0;
    uint64_t last_bd_ver_  = 0;
    uint64_t last_bst_ver_ = 0;

    // Pre-loaded static config (sent to each client on connect)
    std::string module_geo_json_;
    std::string gui_config_json_;
    std::string daq_map_json_;
};
