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
#include "channel_classifier.h"
#include "fault_tracker.h"
#include "fault_logger.h"
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

    // ── Fault log ring buffer ────────────────────────────────────────────
    // Called from the fault logger (any thread) when a transition is logged.
    void pushFaultLogEntry(json entry) {
        std::lock_guard lk(mu_);
        ++fault_log_ver_;
        entry["_seq"] = fault_log_ver_;  // tag for precise incremental retrieval
        fault_log_.push_back(std::move(entry));
        if (fault_log_.size() > max_fault_log_)
            fault_log_.pop_front();
    }

    // Return the full log (for initial client sync)
    std::pair<std::string, uint64_t> getFaultLog() const {
        std::lock_guard lk(mu_);
        json arr = json::array();
        for (const auto &e : fault_log_) arr.push_back(e);
        return { arr.dump(), fault_log_ver_ };
    }

    // Return only entries added since the given version
    std::pair<std::string, uint64_t> getFaultLogSince(uint64_t since_ver) const {
        std::lock_guard lk(mu_);
        if (since_ver >= fault_log_ver_)
            return { "[]", fault_log_ver_ };

        // Walk backwards from the end to find entries with _seq > since_ver
        json arr = json::array();
        for (auto it = fault_log_.rbegin(); it != fault_log_.rend(); ++it) {
            uint64_t seq = it->value("_seq", uint64_t(0));
            if (seq <= since_ver) break;  // all earlier entries are older
            arr.push_back(*it);
        }
        // arr is newest-first; reverse to oldest-first (matching getFaultLog order)
        std::reverse(arr.begin(), arr.end());
        return { arr.dump(), fault_log_ver_ };
    }

    uint64_t faultLogVersion() const {
        std::lock_guard lk(mu_);
        return fault_log_ver_;
    }

    static constexpr size_t DEFAULT_FAULT_LOG_CAP = 200;

    void setFaultLogCapacity(size_t cap) {
        std::lock_guard lk(mu_);
        max_fault_log_ = (cap < 10) ? 10 : cap;  // enforce minimum of 10
        while (fault_log_.size() > max_fault_log_)
            fault_log_.pop_front();
    }

    size_t faultLogCapacity() const {
        std::lock_guard lk(mu_);
        return max_fault_log_;
    }

private:
    mutable std::mutex mu_;
    std::string hv_    = "[]";
    std::string board_ = "[]";
    std::string bst_   = "[]";
    uint64_t hv_ver_    = 0;
    uint64_t board_ver_ = 0;
    uint64_t bst_ver_   = 0;

    std::deque<json> fault_log_;
    uint64_t fault_log_ver_ = 0;
    size_t   max_fault_log_ = DEFAULT_FAULT_LOG_CAP;
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
        : crate_defs_(crate_defs), poll_interval_ms_(3000) {}

    ~HVPoller() {
        for (auto *c : crates_) delete c;
    }

    void setFaultLogger(FaultLogger *logger) { fault_tracker_.setLogger(logger); }

    void setClassifier(const ChannelClassifier &cls) { classifier_ = cls; }
    ChannelClassifier &classifier() { return classifier_; }

    void setPollInterval(int ms) { poll_interval_ms_ = (ms < 500) ? 500 : ms; }
    int  pollInterval() const { return poll_interval_ms_; }

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
            } else {
                std::cerr << fmt::format("Cannot connect to {:s} @ {:s}\n",
                                         cr->GetName(), cr->GetIP());
            }
        }
        std::cout << fmt::format("Init DONE - {}/{} crates OK\n",
                                 ok, crates_.size());
        return (ok == static_cast<int>(crates_.size()));
    }

    // ── Main poll loop (call from a std::thread) ─────────────────────────
    void run(SnapshotStore &store, CommandQueue &cmdq, std::atomic<bool> &running)
    {
        using clock = std::chrono::steady_clock;

        while (running) {
            auto t0 = clock::now();

            // Drain pending HV commands
            while (auto cmd = cmdq.tryPop(Command::HV)) {
                dispatchCommand(cmd->payload);
            }

            // Poll all crates
            doPoll();

            // Publish snapshots
            store.setHV(buildChannelSnapshot());
            store.setBoard(buildBoardSnapshot());

            // Sleep for remainder of interval
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                clock::now() - t0);
            auto sleep_ms = std::chrono::milliseconds(poll_interval_ms_) - elapsed;
            if (sleep_ms.count() > 0 && running) {
                std::this_thread::sleep_for(sleep_ms);
            }
        }
    }

private:
    void doPoll()
    {
        for (auto *cr : crates_) {
            cr->ReadAllParams();
        }
        // Track fault transitions (hardware errors + ΔV warnings)
        for (auto *cr : crates_) {
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

    void dispatchCommand(const json &cmd)
    {
        std::string type = cmd.value("type", "");

        if (type == "set_power") {
            auto *ch = findChannel(cmd.value("crate", ""),
                                   cmd.value("slot", -1),
                                   cmd.value("ch", -1));
            if (ch) ch->SetPower(cmd.value("on", false));
        }
        else if (type == "set_all_power") {
            bool on = cmd.value("on", false);
            for (auto *cr : crates_)
                for (auto *bd : cr->GetBoardList())
                    for (auto *ch : bd->GetChannelList())
                        ch->SetPower(on);
        }
        else if (type == "set_voltage") {
            auto *ch = findChannel(cmd.value("crate", ""),
                                   cmd.value("slot", -1),
                                   cmd.value("ch", -1));
            if (ch) ch->SetVoltage(cmd.value("value", 0.0f));
        }
        else if (type == "set_current") {
            auto *ch = findChannel(cmd.value("crate", ""),
                                   cmd.value("slot", -1),
                                   cmd.value("ch", -1));
            if (ch) ch->SetCurrent(cmd.value("value", 0.0f));
        }
        else if (type == "set_svmax") {
            auto *ch = findChannel(cmd.value("crate", ""),
                                   cmd.value("slot", -1),
                                   cmd.value("ch", -1));
            if (ch) ch->SetSVMax(cmd.value("value", 0.0f));
        }
        else if (type == "set_name") {
            auto *ch = findChannel(cmd.value("crate", ""),
                                   cmd.value("slot", -1),
                                   cmd.value("ch", -1));
            if (ch) ch->SetName(cmd.value("name", ""));
        }
        else if (type == "set_poll_interval") {
            setPollInterval(cmd.value("ms", 3000));
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

    // ── Data ─────────────────────────────────────────────────────────────
    std::vector<CrateDef> crate_defs_;
    std::vector<CAEN_Crate*> crates_;
    std::map<std::string, CAEN_Crate*> crate_map_;
    std::atomic<int> poll_interval_ms_;
    FaultTracker fault_tracker_;
    ChannelClassifier classifier_;
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
            supplies_[idx]->setVoltage(cmd.value("value", 0.0));
        }
        else if (type == "booster_set_current" && validIdx(idx)) {
            supplies_[idx]->setCurrent(cmd.value("value", 0.0));
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
};
