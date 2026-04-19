#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// prad2hvd – PRad-II HV Daemon (pure C++, no Qt)
//
// Core components:
//   SnapshotStore  – thread-safe latest-snapshot buffer
//   CommandQueue   – thread-safe command FIFO
//   HVPoller       – CAEN crate polling loop (runs on its own thread)
//   BoosterPoller  – TDK-Lambda supply polling loop (runs on its own thread)
//
// All JSON serialisation uses nlohmann/json.
// ─────────────────────────────────────────────────────────────────────────────

#include <nlohmann/json.hpp>
#include <caen_channel.h>
#include "booster_supply.h"
#include "vmon_recorder.h"
#include "channel_classifier.h"
#include "fault_tracker.h"
#include "fault_logger.h"
#include "settings_auto_logger.h"
#include "settings_edit_logger.h"
#include <fmt/format.h>

#include <atomic>
#include <algorithm>
#include <chrono>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>
#include <iostream>
#include <cmath>
#include <cstdio>

using json = nlohmann::json;


// ═════════════════════════════════════════════════════════════════════════════
//  SnapshotStore – thread-safe buffer for the latest poll snapshots
// ═════════════════════════════════════════════════════════════════════════════
class SnapshotStore
{
public:
    void setHV(std::string s) {
        std::lock_guard lk(mu_);
        hv_ = std::move(s);
        ++hv_ver_;
    }
    void setBoard(std::string s) {
        std::lock_guard lk(mu_);
        board_ = std::move(s);
        ++board_ver_;
    }
    void setBooster(std::string s) {
        std::lock_guard lk(mu_);
        bst_ = std::move(s);
        ++bst_ver_;
    }
    void setCrateStatus(std::string s) {
        std::lock_guard lk(mu_);
        crate_status_ = std::move(s);
        ++crate_status_ver_;
    }

    // Fast VMon-only snapshot (lightweight, high-frequency)
    void setVMon(std::string s, int64_t ts_ms) {
        std::lock_guard lk(mu_);
        vmon_ = std::move(s);
        vmon_ts_ = ts_ms;
        ++vmon_ver_;
    }
    std::pair<std::string, uint64_t> getVMon() const {
        std::lock_guard lk(mu_);
        return { vmon_, vmon_ver_ };
    }
    int64_t getVMonTs() const {
        std::lock_guard lk(mu_);
        return vmon_ts_;
    }

    std::pair<std::string, uint64_t> getHV() const {
        std::lock_guard lk(mu_);
        return { hv_, hv_ver_ };
    }
    std::pair<std::string, uint64_t> getBoard() const {
        std::lock_guard lk(mu_);
        return { board_, board_ver_ };
    }
    std::pair<std::string, uint64_t> getBooster() const {
        std::lock_guard lk(mu_);
        return { bst_, bst_ver_ };
    }
    std::pair<std::string, uint64_t> getCrateStatus() const {
        std::lock_guard lk(mu_);
        return { crate_status_, crate_status_ver_ };
    }

    // ── Fault log ring buffers (separate for FAULT and WARN) ──────────
    // Called from the fault logger (any thread) when a transition is logged.
    // Routes the entry to the correct buffer based on entry["level"].
    void pushFaultLogEntry(json entry) {
        std::lock_guard lk(mu_);
        const bool isFault = (entry.value("level", "") == "FAULT");
        auto &buf = isFault ? fl_faults_    : fl_warns_;
        auto &ver = isFault ? fl_fault_ver_ : fl_warn_ver_;
        ++ver;
        entry["_seq"] = ver;
        buf.push_back(std::move(entry));
        if (buf.size() > max_fault_log_)
            buf.pop_front();
    }

    // ── Fault buffer accessors ───────────────────────────────────────
    std::pair<std::string, uint64_t> getFaultLogFaults() const {
        std::lock_guard lk(mu_);
        return { dumpDeque_(fl_faults_), fl_fault_ver_ };
    }
    std::pair<std::string, uint64_t> getFaultLogWarns() const {
        std::lock_guard lk(mu_);
        return { dumpDeque_(fl_warns_), fl_warn_ver_ };
    }

    std::pair<std::string, uint64_t> getFaultLogFaultsSince(uint64_t since) const {
        std::lock_guard lk(mu_);
        return { dumpSince_(fl_faults_, since, fl_fault_ver_), fl_fault_ver_ };
    }
    std::pair<std::string, uint64_t> getFaultLogWarnsSince(uint64_t since) const {
        std::lock_guard lk(mu_);
        return { dumpSince_(fl_warns_, since, fl_warn_ver_), fl_warn_ver_ };
    }

    uint64_t faultLogFaultsVersion() const {
        std::lock_guard lk(mu_);
        return fl_fault_ver_;
    }
    uint64_t faultLogWarnsVersion() const {
        std::lock_guard lk(mu_);
        return fl_warn_ver_;
    }

    static constexpr size_t DEFAULT_FAULT_LOG_CAP = 200;

    void setFaultLogCapacity(size_t cap) {
        std::lock_guard lk(mu_);
        max_fault_log_ = (cap < 10) ? 10 : cap;
        while (fl_faults_.size() > max_fault_log_) fl_faults_.pop_front();
        while (fl_warns_.size()  > max_fault_log_) fl_warns_.pop_front();
    }

    size_t faultLogCapacity() const {
        std::lock_guard lk(mu_);
        return max_fault_log_;
    }

    // ── Settings save/load (request-response via HVPoller) ─────────────
    // The WsServer pushes a request; HVPoller processes it and stores
    // the result; WsServer picks it up and sends to the requesting client.
    void setSettingsResponse(std::string json_str, std::string saved_path = "") {
        std::lock_guard lk(mu_);
        settings_response_ = std::move(json_str);
        settings_saved_path_ = std::move(saved_path);
        settings_response_ready_ = true;
    }

    // Returns the response and clears it. Empty string if not ready.
    std::string takeSettingsResponse() {
        std::lock_guard lk(mu_);
        if (!settings_response_ready_) return {};
        settings_response_ready_ = false;
        return std::move(settings_response_);
    }

    std::string takeSettingsSavedPath() {
        std::lock_guard lk(mu_);
        return std::move(settings_saved_path_);
    }

    bool hasSettingsResponse() const {
        std::lock_guard lk(mu_);
        return settings_response_ready_;
    }

    void setLoadResponse(std::string json_str) {
        std::lock_guard lk(mu_);
        load_response_ = std::move(json_str);
        load_response_ready_ = true;
    }

    std::string takeLoadResponse() {
        std::lock_guard lk(mu_);
        if (!load_response_ready_) return {};
        load_response_ready_ = false;
        return std::move(load_response_);
    }

    bool hasLoadResponse() const {
        std::lock_guard lk(mu_);
        return load_response_ready_;
    }

private:
    // ── Helpers for fault log serialisation ───────────────────────────
    static std::string dumpDeque_(const std::deque<json> &dq) {
        json arr = json::array();
        for (const auto &e : dq) arr.push_back(e);
        return arr.dump();
    }
    static std::string dumpSince_(const std::deque<json> &dq,
                                   uint64_t since_ver, uint64_t cur_ver) {
        if (since_ver >= cur_ver) return "[]";
        json arr = json::array();
        for (auto it = dq.rbegin(); it != dq.rend(); ++it) {
            uint64_t seq = it->value("_seq", uint64_t(0));
            if (seq <= since_ver) break;
            arr.push_back(*it);
        }
        std::reverse(arr.begin(), arr.end());
        return arr.dump();
    }

    mutable std::mutex mu_;
    std::string hv_    = "[]";
    std::string board_ = "[]";
    std::string bst_   = "[]";
    std::string vmon_  = "[]";   // fast VMon-only snapshot
    uint64_t hv_ver_    = 0;
    uint64_t board_ver_ = 0;
    uint64_t bst_ver_   = 0;
    uint64_t vmon_ver_  = 0;
    int64_t  vmon_ts_   = 0;     // epoch-ms when VMon was read
    std::string crate_status_ = "[]";
    uint64_t crate_status_ver_ = 0;

    // Two separate fault log ring buffers
    std::deque<json> fl_faults_;
    std::deque<json> fl_warns_;
    uint64_t fl_fault_ver_ = 0;
    uint64_t fl_warn_ver_  = 0;
    size_t   max_fault_log_ = DEFAULT_FAULT_LOG_CAP;

    std::string settings_response_;
    std::string settings_saved_path_;
    bool settings_response_ready_ = false;
    std::string load_response_;
    bool load_response_ready_ = false;
};


// ═════════════════════════════════════════════════════════════════════════════
//  CommandQueue – thread-safe FIFO of JSON commands from clients
// ═════════════════════════════════════════════════════════════════════════════
struct Command {
    enum Target { HV, Booster } target;
    json payload;
};

class CommandQueue
{
public:
    void push(Command cmd) {
        std::lock_guard lk(mu_);
        queue_.push_back(std::move(cmd));
    }

    std::optional<Command> tryPop() {
        std::lock_guard lk(mu_);
        if (queue_.empty()) return std::nullopt;
        auto cmd = std::move(queue_.front());
        queue_.pop_front();
        return cmd;
    }

    // Pop only commands matching the given target.
    // Non-matching commands are left in the queue.
    std::optional<Command> tryPop(Command::Target t) {
        std::lock_guard lk(mu_);
        for (auto it = queue_.begin(); it != queue_.end(); ++it) {
            if (it->target == t) {
                auto cmd = std::move(*it);
                queue_.erase(it);
                return cmd;
            }
        }
        return std::nullopt;
    }

private:
    std::mutex mu_;
    std::deque<Command> queue_;
};


// ═════════════════════════════════════════════════════════════════════════════
//  HVPoller – CAEN crate polling (runs on a dedicated thread)
// ═════════════════════════════════════════════════════════════════════════════
class HVPoller
{
public:
    using CrateDef = std::pair<std::string, std::string>;  // {name, ip}

    explicit HVPoller(const std::vector<CrateDef> &crate_defs)
        : crate_defs_(crate_defs), vmon_poll_ms_(200), all_poll_every_n_(10) {}

    ~HVPoller() {
        for (auto *c : crates_) delete c;
    }

    void setFaultLogger(FaultLogger *logger) { fault_tracker_.setLogger(logger); }
    void setVMonRecorder(VMonRecorder *rec) { vmon_recorder_ = rec; }

    void setSettingsLogDir(const std::string &dir) {
        settings_auto_logger_ = SettingsAutoLogger(dir);
        settings_edit_logger_ = SettingsEditLogger(dir);
    }

    // Directory for Save Settings snapshots ("hv_settings_<ts>.json").  Kept
    // separate from the settings_log dir so the two can live under the same
    // or different roots as the operator prefers.
    void setHvSettingsDir(const std::string &dir) {
        namespace fs = std::filesystem;
        hv_settings_dir_ = dir;
        std::error_code ec;
        fs::create_directories(hv_settings_dir_, ec);
        if (ec) {
            std::cerr << "HVPoller: cannot create hv_settings dir '"
                      << hv_settings_dir_ << "': " << ec.message() << "\n";
        }
    }

    void setClassifier(const ChannelClassifier &cls) { classifier_ = cls; }
    ChannelClassifier &classifier() { return classifier_; }

    const std::vector<CAEN_Crate*> &crates() const { return crates_; }

    void setVMonPollInterval(int ms) { vmon_poll_ms_ = (ms < 50) ? 50 : ms; }
    int  vmonPollInterval() const { return vmon_poll_ms_; }
    void setAllPollEveryN(int n) { all_poll_every_n_ = (n < 1) ? 1 : n; }
    int  allPollEveryN() const { return all_poll_every_n_; }
    // Backward-compat: set the effective full-poll interval
    void setPollInterval(int ms) { vmon_poll_ms_ = (ms < 50) ? 50 : ms; }

    // Must be called before starting the poll thread.
    bool initCrates()
    {
        int crid = 0;
        for (const auto &[name, ip] : crate_defs_) {
            auto *cr = new CAEN_Crate(crid++, name, ip,
                                      CAENHV::SY1527, LINKTYPE_TCPIP,
                                      "admin", "admin");
            crates_.push_back(cr);
            crate_map_[name] = cr;
        }
        int ok = 0;
        for (auto *cr : crates_) {
            if (cr->Initialize()) {
                std::cout << fmt::format("Connected to {:s} @ {:s}\n",
                                         cr->GetName(), cr->GetIP());
                cr->PrintCrateMap();
                ++ok;
                crate_reconnect_[cr->GetName()] = {true, 1, 0};
            } else {
                std::cerr << fmt::format("Cannot connect to {:s} @ {:s}\n",
                                         cr->GetName(), cr->GetIP());
                crate_reconnect_[cr->GetName()] = {false, 1, 1};  // retry next cycle
            }
        }
        std::cout << fmt::format("Init DONE - {}/{} crates OK\n",
                                 ok, crates_.size());
        // Return true even if not all connected — we'll reconnect in the poll loop
        return true;
    }

    // ── Main poll loop (call from a std::thread) ─────────────────────────
    // Dual-speed: fast VMon-only reads every vmon_poll_ms_, full
    // ReadAllParams every all_poll_every_n_ fast cycles.
    void run(SnapshotStore &store, CommandQueue &cmdq, std::atomic<bool> &running)
    {
        using clock   = std::chrono::steady_clock;
        using sys_clk = std::chrono::system_clock;
        int cycle = 0;

        while (running) {
            auto t0 = clock::now();

            // Drain pending HV commands (every cycle for responsiveness)
            while (auto cmd = cmdq.tryPop(Command::HV)) {
                std::string type = cmd->payload.value("type", "");
                if (type == "save_settings") {
                    std::string snapshot = buildSettingsSnapshot();
                    std::string savedPath = saveSettingsFile(snapshot);
                    store.setSettingsResponse(std::move(snapshot), std::move(savedPath));
                } else if (type == "load_settings") {
                    json before = captureBeforeEdit(cmd->payload);
                    if (!before.empty())
                        settings_edit_logger_.recordEdit("load_settings", before);
                    store.setLoadResponse(loadSettings(cmd->payload));
                    if (vmon_recorder_) vmon_recorder_->onNameChange();
                } else {
                    json before = captureBeforeEdit(cmd->payload);
                    if (!before.empty())
                        settings_edit_logger_.recordEdit(type, before);
                    dispatchCommand(cmd->payload);
                }
            }

            const bool fullCycle = (cycle % all_poll_every_n_.load() == 0);

            if (fullCycle) {
                // ── Slow cycle: full parameter read ─────────────────────
                doPoll();
                applyPendingOverrides();
                store.setHV(buildChannelSnapshot());
                store.setBoard(buildBoardSnapshot());
                store.setCrateStatus(buildCrateStatusSnapshot());
                settings_auto_logger_.tick([this]() { return buildSettingsSnapshot(); });
            } else {
                // ── Fast cycle: VMon only ───────────────────────────────
                doFastPoll();
            }

            // Always publish the lightweight VMon snapshot
            auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                sys_clk::now().time_since_epoch()).count();
            store.setVMon(buildVMonSnapshot(), static_cast<int64_t>(now_ms));

            // Record to disk
            if (vmon_recorder_) vmon_recorder_->writeSnapshot();

            ++cycle;

            // Sleep for remainder of the fast interval
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                clock::now() - t0);
            auto sleep_ms = std::chrono::milliseconds(vmon_poll_ms_.load()) - elapsed;
            if (sleep_ms.count() > 0 && running) {
                std::this_thread::sleep_for(sleep_ms);
            }
        }
    }

private:
    void doPoll()
    {
        for (auto *cr : crates_) {
            auto &rs = crate_reconnect_[cr->GetName()];

            if (!rs.connected) {
                // ── Disconnected: count down and try reconnect ──────
                if (--rs.polls_remaining > 0)
                    continue;   // not time yet

                std::cout << fmt::format("Attempting reconnect to {} @ {} ...\n",
                                         cr->GetName(), cr->GetIP());
                if (cr->Reconnect()) {
                    std::cout << fmt::format("Reconnected to {} @ {}\n",
                                             cr->GetName(), cr->GetIP());
                    rs.connected     = true;
                    rs.backoff_polls = 1;
                    rs.polls_remaining = 0;
                } else {
                    // Exponential backoff
                    rs.backoff_polls = std::min(rs.backoff_polls * 2,
                                                CrateReconnectState::MAX_BACKOFF);
                    rs.polls_remaining = rs.backoff_polls;
                    std::cerr << fmt::format("Reconnect failed for {} — "
                                             "retry in {} poll cycles\n",
                                             cr->GetName(), rs.backoff_polls);
                    continue;   // skip polling this crate
                }
            }

            // ── Connected: heartbeat check then poll ────────────────
            if (!cr->HeartBeat()) {
                std::cerr << fmt::format("Lost connection to {} @ {}\n",
                                         cr->GetName(), cr->GetIP());
                rs.connected       = false;
                rs.backoff_polls   = 1;
                rs.polls_remaining = 1;   // try immediately on next cycle
                continue;  // skip ReadAllParams — crate is down
            }

            cr->ReadAllParams();
        }

        // Track fault transitions (only for connected crates)
        for (auto *cr : crates_) {
            if (!crate_reconnect_[cr->GetName()].connected) continue;

            for (auto *bd : cr->GetBoardList()) {
                std::string bdId = cr->GetName() + "_s" + std::to_string(bd->GetSlot());
                fault_tracker_.update(bdId, bd->GetBdStatusString(), "board");
                for (auto *ch : bd->GetChannelList()) {
                    // Classify the channel on the daemon side
                    auto cls = classifier_.classify(
                        ch->GetStatusString(),
                        ch->GetVMon(), ch->GetVSet(),
                        ch->IsTurnedOn(), ch->GetName());

                    // Build a combined status string for fault tracking.
                    // If ΔV warning is active, append a synthetic "DVW" token
                    // so the fault tracker logs it as a transition.
                    std::string trackStatus = ch->GetStatusString();
                    if (cls.dv_warn) {
                        std::string abbr = trackStatus;
                        auto pipe = abbr.find('|');
                        if (pipe != std::string::npos)
                            trackStatus = abbr.substr(0, pipe) + " DVW|"
                                        + abbr.substr(pipe + 1) + ", dV warning";
                        else
                            trackStatus += " DVW|dV warning";
                    }

                    // Determine log level: real hardware faults → FAULT,
                    // ΔV warnings and suppressed errors → WARN
                    FaultLogger::Level logLevel = FaultLogger::Level::Fault;
                    if (cls.level == "warn" || cls.level == "suppressed")
                        logLevel = FaultLogger::Level::Warn;
                    // If the only "fault" is DVW (ΔV), it's a warning
                    if (cls.dv_warn && cls.level != "fault")
                        logLevel = FaultLogger::Level::Warn;

                    fault_tracker_.update(ch->GetName(), trackStatus, "channel", logLevel,
                                          ch->GetVMon(), ch->GetVSet());
                }
            }
        }
        fault_tracker_.endCycle();
    }

    // ── Fast VMon-only poll (no reconnect/heartbeat/faults) ─────────────
    void doFastPoll()
    {
        for (auto *cr : crates_) {
            if (!crate_reconnect_[cr->GetName()].connected) continue;
            cr->ReadVMonOnly();
        }
    }

    // ── Lightweight VMon snapshot (manual JSON for speed) ────────────────
    std::string buildVMonSnapshot()
    {
        std::string s;
        s.reserve(60000);   // ~1700 ch × ~30 bytes
        s += '[';
        bool first = true;
        for (auto *cr : crates_) {
            if (!crate_reconnect_[cr->GetName()].connected) continue;
            for (auto *bd : cr->GetBoardList()) {
                for (auto *ch : bd->GetChannelList()) {
                    float v = ch->GetVMon();
                    if (std::isnan(v)) continue;
                    if (!first) s += ',';
                    first = false;
                    s += R"({"n":")";
                    s += ch->GetName();
                    s += R"(","v":)";
                    char buf[16];
                    std::snprintf(buf, sizeof(buf), "%.2f", v);
                    s += buf;
                    s += '}';
                }
            }
        }
        s += ']';
        return s;
    }

    // ── Capture pre-edit state for restore points ─────────────────────
    // Returns a JSON array of channel entries (partial settings) reflecting
    // the current values of params that are about to be changed.  Empty
    // array if nothing to capture (non-edit commands, channel not found).
    json captureBeforeEdit(const json &cmd)
    {
        json channels = json::array();
        std::string type = cmd.value("type", "");

        // Helper: read one channel's current values for the given params
        auto captureSingle = [&](const std::string &crate_name, int slot,
                                 int ch_idx,
                                 const std::vector<std::string> &param_names) {
            auto *ch = findChannel(crate_name, slot, ch_idx);
            if (!ch) return;
            auto cit = crate_map_.find(crate_name);
            if (cit == crate_map_.end()) return;
            auto *bd = cit->second->GetBoard(
                static_cast<unsigned short>(slot));
            if (!bd) return;

            const auto &paramInfo = bd->GetChParamInfo();
            json params;
            for (const auto &pname : param_names) {
                for (const auto &pi : paramInfo) {
                    if (pi.name != pname) continue;
                    if (pi.isFloat()) {
                        float v = ch->GetFloat(pname);
                        if (!std::isnan(v))
                            params[pname] = std::round(v * 100.0f) / 100.0f;
                    } else if (pi.isUInt()) {
                        if (ch->HasParam(pname))
                            params[pname] = ch->GetUInt(pname);
                    }
                    break;
                }
            }
            if (params.empty()) return;

            json entry;
            entry["crate"]   = crate_name;
            entry["slot"]    = slot;
            entry["channel"] = ch_idx;
            entry["name"]    = ch->GetName();
            entry["params"]  = std::move(params);
            channels.push_back(std::move(entry));
        };

        if (type == "set_voltage") {
            captureSingle(cmd.value("crate",""), cmd.value("slot",-1),
                          cmd.value("ch",-1), {"V0Set"});
        }
        else if (type == "set_voltage_by_name") {
            auto addr = findChannelByName(cmd.value("name", ""));
            if (addr.ch)
                captureSingle(addr.crate, addr.slot, addr.channel, {"V0Set"});
        }
        else if (type == "set_current") {
            captureSingle(cmd.value("crate",""), cmd.value("slot",-1),
                          cmd.value("ch",-1), {"I0Set"});
        }
        else if (type == "set_svmax") {
            captureSingle(cmd.value("crate",""), cmd.value("slot",-1),
                          cmd.value("ch",-1), {"SVMax"});
        }
        else if (type == "set_power") {
            captureSingle(cmd.value("crate",""), cmd.value("slot",-1),
                          cmd.value("ch",-1), {"Pw"});
        }
        else if (type == "set_name") {
            auto *ch = findChannel(cmd.value("crate",""),
                                   cmd.value("slot",-1), cmd.value("ch",-1));
            if (ch) {
                json entry;
                entry["crate"]   = cmd.value("crate","");
                entry["slot"]    = cmd.value("slot",-1);
                entry["channel"] = cmd.value("ch",-1);
                entry["name"]    = ch->GetName();
                entry["params"]  = json::object();
                channels.push_back(std::move(entry));
            }
        }
        else if (type == "set_all_power") {
            bool target = cmd.value("on", false);
            unsigned int target_pw = target ? 1u : 0u;
            for (auto *cr : crates_)
                for (auto *bd : cr->GetBoardList())
                    for (auto *ch : bd->GetChannelList()) {
                        if (ch->HasParam("Pw") &&
                            ch->GetUInt("Pw") == target_pw)
                            continue;
                        captureSingle(cr->GetName(), bd->GetSlot(),
                                      ch->GetChannel(), {"Pw"});
                    }
        }
        else if (type == "set_power_batch") {
            bool target = cmd.value("on", false);
            unsigned int target_pw = target ? 1u : 0u;
            if (cmd.contains("channels") && cmd["channels"].is_array()) {
                for (const auto &e : cmd["channels"]) {
                    std::string cr = e.value("crate","");
                    int sl = e.value("slot",-1);
                    int ci = e.value("ch",-1);
                    auto *ch = findChannel(cr, sl, ci);
                    if (ch && ch->HasParam("Pw") &&
                        ch->GetUInt("Pw") == target_pw)
                        continue;
                    captureSingle(cr, sl, ci, {"Pw"});
                }
            }
        }
        else if (type == "set_all_voltage") {
            float target = cmd.value("value", 0.0f);
            for (auto *cr : crates_)
                for (auto *bd : cr->GetBoardList())
                    for (auto *ch : bd->GetChannelList()) {
                        float cur = ch->GetVSet();
                        if (!std::isnan(cur) &&
                            std::fabs(cur - target) < 0.01f)
                            continue;
                        captureSingle(cr->GetName(), bd->GetSlot(),
                                      ch->GetChannel(), {"V0Set"});
                    }
        }
        else if (type == "load_settings") {
            // Capture current values for all channels/params the file
            // will try to modify
            json data;
            if (cmd.contains("settings") && cmd["settings"].is_object())
                data = cmd["settings"];
            else if (cmd.contains("settings") &&
                     cmd["settings"].is_string())
                data = json::parse(
                    cmd["settings"].get<std::string>(), nullptr, false);

            if (!data.is_discarded() && data.contains("channels") &&
                data["channels"].is_array()) {
                for (const auto &ch_entry : data["channels"]) {
                    std::string crate = ch_entry.value("crate","");
                    int sl = ch_entry.value("slot",-1);
                    int ci = ch_entry.value("channel",-1);
                    if (!ch_entry.contains("params") ||
                        !ch_entry["params"].is_object())
                        continue;
                    std::vector<std::string> params;
                    for (auto it = ch_entry["params"].begin();
                         it != ch_entry["params"].end(); ++it)
                        params.push_back(it.key());
                    if (!params.empty())
                        captureSingle(crate, sl, ci, params);
                }
            }
        }

        return channels;
    }

    void dispatchCommand(const json &cmd)
    {
        std::string type = cmd.value("type", "");
        std::string crate = cmd.value("crate", "");
        int slot = cmd.value("slot", -1);
        int ch_idx = cmd.value("ch", -1);

        // Guard: skip commands targeting a disconnected crate
        if (!crate.empty()) {
            auto rsIt = crate_reconnect_.find(crate);
            if (rsIt != crate_reconnect_.end() && !rsIt->second.connected) {
                std::cerr << "HVPoller: command skipped — crate "
                          << crate << " is disconnected\n";
                return;
            }
        }

        if (type == "set_power") {
            auto *ch = findChannel(crate, slot, ch_idx);
            if (ch) ch->SetPower(cmd.value("on", false));
        }
        else if (type == "set_all_power") {
            bool on = cmd.value("on", false);
            for (auto *cr : crates_)
                for (auto *bd : cr->GetBoardList())
                    for (auto *ch : bd->GetChannelList())
                        ch->SetPower(on);
        }
        else if (type == "set_power_batch") {
            bool on = cmd.value("on", false);
            if (cmd.contains("channels") && cmd["channels"].is_array()) {
                for (const auto &entry : cmd["channels"]) {
                    std::string cr = entry.value("crate", "");
                    int sl = entry.value("slot", -1);
                    int ci = entry.value("ch", -1);
                    auto *channel = findChannel(cr, sl, ci);
                    if (channel) channel->SetPower(on);
                }
            }
        }
        else if (type == "set_voltage") {
            auto *ch = findChannel(crate, slot, ch_idx);
            if (ch) {
                float v = cmd.value("value", 0.0f);
                ch->SetVoltage(v);
                float actual = std::min(v, ch->GetLimit());
                addPendingOverride(crate, slot, ch_idx, "V0Set", actual);
            }
        }
        else if (type == "set_current") {
            auto *ch = findChannel(crate, slot, ch_idx);
            if (ch) {
                float v = cmd.value("value", 0.0f);
                ch->SetCurrent(v);
                addPendingOverride(crate, slot, ch_idx, "I0Set", v);
            }
        }
        else if (type == "set_svmax") {
            auto *ch = findChannel(crate, slot, ch_idx);
            if (ch) {
                float v = cmd.value("value", 0.0f);
                ch->SetSVMax(v);
                addPendingOverride(crate, slot, ch_idx, "SVMax", v);
            }
        }
        else if (type == "set_name") {
            auto *ch = findChannel(crate, slot, ch_idx);
            if (ch) {
                ch->SetName(cmd.value("name", ""));
                if (vmon_recorder_) vmon_recorder_->onNameChange();
            }
        }
        else if (type == "set_poll_interval") {
            if (cmd.contains("vmon_ms"))
                setVMonPollInterval(cmd.value("vmon_ms", 200));
            if (cmd.contains("all_every_n"))
                setAllPollEveryN(cmd.value("all_every_n", 10));
            if (cmd.contains("ms") && !cmd.contains("vmon_ms"))
                setVMonPollInterval(cmd.value("ms", 200));
        }
        else if (type == "set_all_voltage") {
            float v = cmd.value("value", 0.0f);
            for (auto *cr : crates_)
                for (auto *bd : cr->GetBoardList())
                    for (auto *ch : bd->GetChannelList()) {
                        float cur = ch->GetVSet();
                        if (!std::isnan(cur) && std::fabs(cur - v) < 0.01f) continue;
                        ch->SetVoltage(v);
                    }
        }
        else if (type == "set_voltage_by_name") {
            std::string name = cmd.value("name", "");
            auto addr = findChannelByName(name);
            if (!addr.ch) {
                std::cerr << "HVPoller: set_voltage_by_name — channel \""
                          << name << "\" not found\n";
            } else {
                // Check disconnected-crate guard (same as top-level guard)
                auto rsIt = crate_reconnect_.find(addr.crate);
                if (rsIt != crate_reconnect_.end() && !rsIt->second.connected) {
                    std::cerr << "HVPoller: set_voltage_by_name skipped — crate "
                              << addr.crate << " is disconnected\n";
                } else {
                    float v = cmd.value("value", 0.0f);
                    addr.ch->SetVoltage(v);
                    float actual = std::min(v, addr.ch->GetLimit());
                    addPendingOverride(addr.crate, addr.slot, addr.channel,
                                       "V0Set", actual);
                }
            }
        }
        else if (type == "get_voltage") {
            // Handled synchronously in WsServer::onMessage — should not reach here
        }
        else if (type == "load_settings") {
            loadSettings(cmd);
        }
        else {
            std::cerr << "HVPoller: unknown command type: " << type << "\n";
        }
    }

    CAEN_Channel *findChannel(const std::string &crateName, int slot, int channel)
    {
        auto it = crate_map_.find(crateName);
        if (it == crate_map_.end()) return nullptr;
        auto *bd = it->second->GetBoard(static_cast<unsigned short>(slot));
        if (!bd) return nullptr;
        return bd->GetChannel(channel);
    }

    struct ChannelAddr {
        CAEN_Channel *ch   = nullptr;
        std::string   crate;
        int           slot    = -1;
        int           channel = -1;
    };

    ChannelAddr findChannelByName(const std::string &name)
    {
        for (auto *cr : crates_) {
            for (auto *bd : cr->GetBoardList()) {
                for (auto *ch : bd->GetChannelList()) {
                    if (ch->GetName() == name)
                        return { ch, cr->GetName(), bd->GetSlot(), ch->GetChannel() };
                }
            }
        }
        return {};
    }

    // ── JSON snapshot builders ───────────────────────────────────────────

    static json jsonOrNull(float v) {
        return std::isnan(v) ? json(nullptr) : json(v);
    }

    std::string buildChannelSnapshot()
    {
        json arr = json::array();
        for (auto *cr : crates_) {
            for (auto *bd : cr->GetBoardList()) {
                for (auto *ch : bd->GetChannelList()) {
                    json o;
                    o["crate"]   = cr->GetName();
                    o["ip"]      = cr->GetIP();
                    o["slot"]    = bd->GetSlot();
                    o["model"]   = bd->GetModel();
                    o["channel"] = ch->GetChannel();
                    o["name"]    = ch->GetName();

                    // Emit all discovered params generically
                    for (const auto &[pname, pval] : ch->GetParams()) {
                        if (pval.tag == ParamValue::Float)
                            o[pname] = jsonOrNull(pval.f);
                        else if (pval.tag == ParamValue::UInt)
                            o[pname] = pval.u;
                    }

                    // Frontend-compatible aliases
                    o["vmon"]       = jsonOrNull(ch->GetVMon());
                    o["vset"]       = jsonOrNull(ch->GetVSet());
                    o["svmax"]      = jsonOrNull(ch->GetSVMax());
                    o["imon"]       = jsonOrNull(ch->GetIMon());
                    o["iset"]       = jsonOrNull(ch->GetISet());
                    o["on"]         = ch->IsTurnedOn();
                    o["status"]     = ch->GetStatusString();
                    o["limit"]      = ch->GetLimit();
                    o["iSupported"] = ch->HasParam("IMon");

                    // Daemon-side classification (authoritative status)
                    auto cls = classifier_.classify(
                        ch->GetStatusString(),
                        ch->GetVMon(), ch->GetVSet(),
                        ch->IsTurnedOn(), ch->GetName());
                    o["level"]        = cls.level;
                    o["dv_warn"]      = cls.dv_warn;
                    o["dv_threshold"] = cls.dv_threshold;

                    arr.push_back(std::move(o));
                }
            }
        }
        return arr.dump();
    }

    std::string buildBoardSnapshot()
    {
        json arr = json::array();
        for (auto *cr : crates_) {
            for (auto *bd : cr->GetBoardList()) {
                json o;
                o["crate"]    = cr->GetName();
                o["slot"]     = bd->GetSlot();
                o["model"]    = bd->GetModel();
                o["desc"]     = bd->GetDescription();
                o["nChan"]    = bd->GetSize();
                o["serial"]   = bd->GetSerialNum();
                o["firmware"] = bd->GetFirmware();

                // All discovered board params
                for (const auto &[pname, pval] : bd->GetBdParams()) {
                    if (pval.tag == ParamValue::Float)
                        o[pname] = jsonOrNull(pval.f);
                    else if (pval.tag == ParamValue::UInt)
                        o[pname] = pval.u;
                }

                // Frontend-compatible aliases
                o["hvmax"]    = jsonOrNull(bd->GetHVMax());
                o["temp"]     = jsonOrNull(bd->GetTemp());
                o["bdstatus"] = bd->GetBdStatusString();

                arr.push_back(std::move(o));
            }
        }
        return arr.dump();
    }

    // ── Save settings snapshot to file ─────────────────────────────────
    std::string saveSettingsFile(const std::string &snapshot)
    {
        namespace fs = std::filesystem;
        if (hv_settings_dir_.empty()) {
            std::cerr << "*** WARNING: hv_settings directory not configured; "
                         "settings snapshot dropped\n";
            return {};
        }
        std::error_code ec;
        fs::create_directories(hv_settings_dir_, ec);
        if (ec) {
            std::cerr << "*** WARNING: cannot create hv_settings dir '"
                      << hv_settings_dir_ << "': " << ec.message()
                      << " — snapshot dropped\n";
            return {};
        }

        auto now = std::chrono::system_clock::now();
        auto tt  = std::chrono::system_clock::to_time_t(now);
        std::tm local{};
        localtime_r(&tt, &local);
        char ts[32];
        std::snprintf(ts, sizeof(ts), "%04d%02d%02d_%02d%02d%02d",
                      local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                      local.tm_hour, local.tm_min, local.tm_sec);

        fs::path path = fs::path(hv_settings_dir_) /
                        fmt::format("hv_settings_{}.json", ts);
        std::ofstream f(path, std::ios::trunc);
        if (f) {
            f << snapshot << "\n";
            std::cout << fmt::format("Settings saved to {}\n", path.string());
            return path.string();
        }
        std::cerr << "*** WARNING: failed to save settings to "
                  << path.string() << "\n";
        return {};
    }

    // ── Crate connection status snapshot ────────────────────────────────
    std::string buildCrateStatusSnapshot()
    {
        json arr = json::array();
        for (auto *cr : crates_) {
            auto &rs = crate_reconnect_[cr->GetName()];
            json o;
            o["name"]      = cr->GetName();
            o["ip"]        = cr->GetIP();
            o["connected"] = rs.connected;
            arr.push_back(std::move(o));
        }
        return arr.dump();
    }

    // ── Settings save: build JSON of all writable params ─────────────────
    std::string buildSettingsSnapshot()
    {
        json root;
        root["format"]    = "prad2hvmon_settings_v1";
        auto now = std::chrono::system_clock::now();
        auto tt  = std::chrono::system_clock::to_time_t(now);
        char tbuf[32];
        std::strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", std::localtime(&tt));
        root["timestamp"] = tbuf;

        json channels = json::array();
        int total = 0;

        for (auto *cr : crates_) {
            for (auto *bd : cr->GetBoardList()) {
                const auto &paramInfo = bd->GetChParamInfo();
                for (auto *ch : bd->GetChannelList()) {
                    json entry;
                    entry["crate"]   = cr->GetName();
                    entry["slot"]    = bd->GetSlot();
                    entry["channel"] = ch->GetChannel();
                    entry["name"]    = ch->GetName();

                    json params;
                    for (const auto &pi : paramInfo) {
                        if (!pi.isWritable()) continue;
                        if (pi.isFloat()) {
                            float v = ch->GetFloat(pi.name);
                            if (!std::isnan(v)) {
                                // Round to 2 decimal places for clean JSON
                                float rounded = std::round(v * 100.0f) / 100.0f;
                                params[pi.name] = rounded;
                            }
                        } else if (pi.isUInt()) {
                            if (ch->HasParam(pi.name))
                                params[pi.name] = ch->GetUInt(pi.name);
                        }
                    }
                    entry["params"] = params;
                    channels.push_back(std::move(entry));
                    ++total;
                }
            }
        }
        root["channels"] = channels;
        std::cout << fmt::format("Settings snapshot: {} channels\n", total);
        return root.dump(2);
    }

    // ── Settings load: restore writable params from JSON ─────────────────
    std::string loadSettings(const json &cmd)
    {
        json data;
        if (cmd.contains("settings") && cmd["settings"].is_object())
            data = cmd["settings"];
        else if (cmd.contains("settings") && cmd["settings"].is_string())
            data = json::parse(cmd["settings"].get<std::string>(), nullptr, false);
        else {
            std::cerr << "CMD load_settings: missing 'settings' field\n";
            return R"({"error":"missing settings field"})";
        }
        if (data.is_discarded()) {
            std::cerr << "CMD load_settings: failed to parse settings JSON\n";
            return R"({"error":"invalid JSON"})";
        }

        std::string format = data.value("format", "");
        if (format != "prad2hvmon_settings_v1")
            std::cerr << "CMD load_settings: unknown format '" << format << "'\n";
        std::cout << "CMD load_settings: timestamp="
                  << data.value("timestamp", "?") << "\n";

        auto &ch_arr = data["channels"];
        if (!ch_arr.is_array()) {
            std::cerr << "CMD load_settings: 'channels' is not an array\n";
            return R"({"error":"channels is not an array"})";
        }

        int restored = 0, skipped = 0, errors = 0, unchanged = 0;

        for (const auto &entry : ch_arr) {
            std::string crate_name = entry.value("crate", "");
            int slot_n    = entry.value("slot", -1);
            int channel_n = entry.value("channel", -1);
            std::string ch_name = entry.value("name", "");

            auto *ch = findChannel(crate_name, slot_n, channel_n);
            if (!ch) {
                ++skipped;
                continue;
            }

            if (!ch_name.empty() && ch->GetName() != ch_name) {
                ch->SetName(ch_name);
                std::cout << fmt::format("  {}/s{}/ch{} name → {}\n",
                    crate_name, slot_n, channel_n, ch_name);
            }

            auto cit = crate_map_.find(crate_name);
            if (cit == crate_map_.end()) { ++skipped; continue; }
            auto *board = cit->second->GetBoard(static_cast<unsigned short>(slot_n));
            if (!board) { ++skipped; continue; }
            const auto &paramInfo = board->GetChParamInfo();

            if (!entry.contains("params") || !entry["params"].is_object()) continue;
            for (auto it = entry["params"].begin(); it != entry["params"].end(); ++it) {
                std::string pname = it.key();
                const ParamInfo *pi = nullptr;
                for (const auto &info : paramInfo) {
                    if (info.name == pname && info.isWritable()) { pi = &info; break; }
                }
                if (!pi) continue;

                bool ok = false;
                if (pi->isFloat()) {
                    float v = it.value().get<float>();
                    float cur = ch->GetFloat(pname);
                    if (!std::isnan(cur) && std::fabs(cur - v) < 0.01f) { ++unchanged; continue; }
                    ok = ch->SetFloat(pname, v);
                    if (ok) std::cout << fmt::format("  {}/s{}/ch{} {} → {:.2f}\n",
                        crate_name, slot_n, channel_n, pname, v);
                } else if (pi->isUInt()) {
                    unsigned int v = it.value().get<unsigned int>();
                    unsigned int cur = ch->GetUInt(pname);
                    if (ch->HasParam(pname) && cur == v) { ++unchanged; continue; }
                    ok = ch->SetUInt(pname, v);
                    if (ok) std::cout << fmt::format("  {}/s{}/ch{} {} → {}\n",
                        crate_name, slot_n, channel_n, pname, v);
                }
                if (ok) ++restored; else ++errors;
            }
        }
        std::cout << fmt::format("CMD load_settings: {} restored, {} unchanged, {} skipped, {} errors\n",
            restored, unchanged, skipped, errors);

        // Build result for client feedback
        json result;
        result["restored"]  = restored;
        result["unchanged"] = unchanged;
        result["skipped"]  = skipped;
        result["errors"]   = errors;
        return result.dump();
    }

    struct PendingOverride {
        std::string crate;
        int         slot;
        int         channel;
        std::string param;      // "V0Set", "I0Set", "SVMax"
        float       value;
        int         ttl;        // poll cycles remaining
    };

    static constexpr int   OVERRIDE_TTL = 20;      // ~20 poll cycles safety backstop
    static constexpr float OVERRIDE_TOL = 0.01f;   // match tolerance

    void addPendingOverride(const std::string &crate, int slot, int ch,
                            const std::string &param, float value)
    {
        // Replace existing override for the same channel+param
        for (auto &po : pending_overrides_) {
            if (po.crate == crate && po.slot == slot &&
                po.channel == ch && po.param == param) {
                po.value = value;
                po.ttl   = OVERRIDE_TTL;
                return;
            }
        }
        pending_overrides_.push_back({crate, slot, ch, param, value, OVERRIDE_TTL});
    }

    // Call once per poll cycle, AFTER doPoll() and BEFORE buildChannelSnapshot().
    // Decrements TTL, removes expired/converged entries, then re-applies
    // surviving overrides into the CAEN param cache.
    void applyPendingOverrides()
    {
        for (auto it = pending_overrides_.begin(); it != pending_overrides_.end(); ) {
            auto *ch = findChannel(it->crate, it->slot, it->channel);
            if (!ch) { it = pending_overrides_.erase(it); continue; }

            float hw = ch->GetFloat(it->param);
            if (!std::isnan(hw) && std::fabs(hw - it->value) < OVERRIDE_TOL) {
                it = pending_overrides_.erase(it);
                continue;
            }

            if (--it->ttl <= 0) {
                it = pending_overrides_.erase(it);
                continue;
            }

            ch->SetParamDirect(it->param, it->value);
            ++it;
        }
    }

    // ── Per-crate reconnect state ───────────────────────────────────────
    struct CrateReconnectState {
        bool   connected       = true;
        int    backoff_polls   = 1;      // current backoff (in poll cycles)
        int    polls_remaining = 0;      // countdown to next reconnect attempt
        static constexpr int MAX_BACKOFF = 30; // ~90s at 3s poll
    };

    // ── Data ─────────────────────────────────────────────────────────────
    std::vector<CrateDef> crate_defs_;
    std::vector<CAEN_Crate*> crates_;
    std::map<std::string, CAEN_Crate*> crate_map_;
    std::map<std::string, CrateReconnectState> crate_reconnect_;
    std::atomic<int> vmon_poll_ms_;        // fast VMon-only interval
    std::atomic<int> all_poll_every_n_;   // full poll every N fast cycles
    FaultTracker fault_tracker_;
    ChannelClassifier classifier_;
    VMonRecorder *vmon_recorder_ = nullptr;   // optional, not owned
    std::vector<PendingOverride> pending_overrides_;
    SettingsAutoLogger settings_auto_logger_;
    SettingsEditLogger settings_edit_logger_;
    std::string hv_settings_dir_;             // set by setHvSettingsDir()
};


// ═════════════════════════════════════════════════════════════════════════════
//  BoosterPoller – TDK-Lambda supply polling (runs on a dedicated thread)
// ═════════════════════════════════════════════════════════════════════════════
class BoosterPoller
{
public:
    struct SupplyDef {
        std::string name;
        std::string ip;
        uint16_t    port = 8003;
    };

    explicit BoosterPoller(const std::vector<SupplyDef> &defs)
        : poll_interval_ms_(3000)
    {
        for (const auto &d : defs) {
            auto *s  = new BoosterSupply();
            s->name  = d.name;
            s->ip    = d.ip;
            s->port  = d.port;
            supplies_.push_back(s);
        }
    }

    ~BoosterPoller() {
        for (auto *s : supplies_) delete s;
    }

    void setFaultLogger(FaultLogger *logger) { fault_tracker_.setLogger(logger); }
    void setPollInterval(int ms) { poll_interval_ms_ = (ms < 500) ? 500 : ms; }

    const std::vector<BoosterSupply*> &supplies() const { return supplies_; }

    // ── Main poll loop (call from a std::thread) ─────────────────────────
    void run(SnapshotStore &store, CommandQueue &cmdq, std::atomic<bool> &running)
    {
        using clock = std::chrono::steady_clock;

        while (running) {
            auto t0 = clock::now();

            // Drain pending booster commands
            while (auto cmd = cmdq.tryPop(Command::Booster)) {
                dispatchCommand(cmd->payload);
            }

            // Poll all supplies and log connection state changes
            for (auto *s : supplies_) {
                bool wasCon = s->connected;
                s->poll();
                if (!wasCon && s->connected) {
                    std::cout << fmt::format("Booster {} @ {}:{} — connected\n",
                                             s->name, s->ip, s->port);
                } else if (wasCon && !s->connected) {
                    std::cerr << fmt::format("Booster {} @ {}:{} — connection lost: {}\n",
                                             s->name, s->ip, s->port, s->error);
                } else if (!s->connected && !s->error.empty()) {
                    // Still disconnected — only log on first failure,
                    // tracked via prev_booster_logged_
                    if (prev_booster_logged_.find(s->name) == prev_booster_logged_.end()) {
                        std::cerr << fmt::format("Booster {} @ {}:{} — cannot connect: {}\n",
                                                 s->name, s->ip, s->port, s->error);
                        prev_booster_logged_.insert(s->name);
                    }
                }
                if (s->connected) {
                    prev_booster_logged_.erase(s->name);
                }
            }

            // Track faults
            for (auto *s : supplies_) {
                std::string status = s->connected ? "" : s->error;
                fault_tracker_.update(s->name, status, "booster");
            }
            fault_tracker_.endCycle();

            // Re-apply any pending set-value overrides
            applyPendingOverrides();

            // Publish snapshot
            store.setBooster(buildSnapshot());

            // Sleep for remainder
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                clock::now() - t0);
            auto sleep_ms = std::chrono::milliseconds(poll_interval_ms_.load()) - elapsed;
            if (sleep_ms.count() > 0 && running) {
                std::this_thread::sleep_for(sleep_ms);
            }
        }

        // Clean shutdown: close all sockets
        for (auto *s : supplies_) s->closeSocket();
    }

private:
    void dispatchCommand(const json &cmd)
    {
        std::string type = cmd.value("type", "");
        int idx = cmd.value("idx", -1);

        if (type == "booster_set_output" && validIdx(idx)) {
            supplies_[idx]->setOutput(cmd.value("on", false));
        }
        else if (type == "booster_set_voltage" && validIdx(idx)) {
            double v = cmd.value("value", 0.0);
            supplies_[idx]->setVoltage(v);
            addPendingOverride(idx, "vset", v);
        }
        else if (type == "booster_set_current" && validIdx(idx)) {
            double v = cmd.value("value", 0.0);
            supplies_[idx]->setCurrent(v);
            addPendingOverride(idx, "iset", v);
        }
        else if (type == "set_poll_interval") {
            setPollInterval(cmd.value("ms", 3000));
        }
        else {
            std::cerr << "BoosterPoller: unknown command type: " << type << "\n";
        }
    }

    bool validIdx(int idx) const {
        return idx >= 0 && idx < static_cast<int>(supplies_.size());
    }

    std::string buildSnapshot()
    {
        json arr = json::array();
        for (int i = 0; i < static_cast<int>(supplies_.size()); ++i) {
            const auto *s = supplies_[i];
            json o;
            o["idx"]       = i;
            o["name"]      = s->name;
            o["ip"]        = s->ip;
            o["connected"] = s->connected;
            o["on"]        = s->on;
            o["mode"]      = s->mode;
            o["error"]     = s->error;
            o["vmon"]      = std::isnan(s->vmon) ? json(nullptr) : json(s->vmon);
            o["vset"]      = std::isnan(s->vset) ? json(nullptr) : json(s->vset);
            o["imon"]      = std::isnan(s->imon) ? json(nullptr) : json(s->imon);
            o["iset"]      = std::isnan(s->iset) ? json(nullptr) : json(s->iset);
            arr.push_back(std::move(o));
        }
        return arr.dump();
    }

    std::vector<BoosterSupply*> supplies_;
    std::atomic<int> poll_interval_ms_;
    FaultTracker fault_tracker_;
    std::unordered_set<std::string> prev_booster_logged_;  // suppresses repeat "cannot connect" msgs

    // ── Pending overrides (same concept as HVPoller) ─────────────────────
    struct PendingOverride {
        int         idx;
        std::string param;      // "vset" or "iset"
        double      value;
        int         ttl;
    };

    static constexpr int    OVERRIDE_TTL = 20;
    static constexpr double OVERRIDE_TOL = 0.01;

    std::vector<PendingOverride> pending_overrides_;

    void addPendingOverride(int idx, const std::string &param, double value)
    {
        for (auto &po : pending_overrides_) {
            if (po.idx == idx && po.param == param) {
                po.value = value;
                po.ttl   = OVERRIDE_TTL;
                return;
            }
        }
        pending_overrides_.push_back({idx, param, value, OVERRIDE_TTL});
    }

    void applyPendingOverrides()
    {
        for (auto it = pending_overrides_.begin(); it != pending_overrides_.end(); ) {
            if (!validIdx(it->idx)) { it = pending_overrides_.erase(it); continue; }
            auto *s = supplies_[it->idx];

            double hw = (it->param == "vset") ? s->vset : s->iset;
            if (!std::isnan(hw) && std::fabs(hw - it->value) < OVERRIDE_TOL) {
                it = pending_overrides_.erase(it);
                continue;
            }
            if (--it->ttl <= 0) {
                it = pending_overrides_.erase(it);
                continue;
            }
            // Re-stamp the pending value
            if (it->param == "vset") s->vset = it->value;
            else                     s->iset = it->value;
            ++it;
        }
    }
};
