#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// SettingsEditLogger – per-edit restore points for HV settings changes.
//
// Every time an edit command is dispatched, the caller captures the
// pre-edit state of affected channels and calls recordEdit().  The logger
// maintains a rolling history of the last N restore points in memory
// and on disk.
//
// Two files are written to the log directory:
//
//   last_edit_restore.json      – overwritten each edit, contains just the
//                                  most recent before-state (quick undo)
//   edit_restore_history.json   – rolling array of the last N restore points
//
// Both use the standard prad2hvmon_settings_v1 channel format, so any
// entry can be loaded directly via loadSettings().
//
// Thread safety: recordEdit() is called only from the HV poller thread
// (inside dispatchCommand / run), so no mutex is needed.
// ─────────────────────────────────────────────────────────────────────────────

#include <nlohmann/json.hpp>

#include <chrono>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include <fmt/format.h>

class SettingsEditLogger
{
public:
    explicit SettingsEditLogger(const std::string &log_dir = "database/settings_log",
                                size_t max_history = 20)
        : log_dir_(log_dir),
          max_history_(max_history < 1 ? 1 : max_history)
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::create_directories(log_dir_, ec);
        if (ec) {
            std::cerr << "SettingsEditLogger: cannot create directory '"
                      << log_dir_ << "': " << ec.message() << "\n";
        }

        loadHistory();
    }

    // Record the before-state of channels about to be edited.
    //   edit_type : command type string (e.g. "set_voltage", "load_settings")
    //   channels  : JSON array of channel entries with params (partial settings)
    void recordEdit(const std::string &edit_type,
                    const nlohmann::json &channels)
    {
        if (channels.empty()) return;

        nlohmann::json entry;
        entry["format"]    = "prad2hvmon_settings_v1";
        entry["timestamp"] = currentTimestamp();
        entry["edit_type"] = edit_type;
        entry["channels"]  = channels;

        history_.push_back(entry);
        while (history_.size() > max_history_)
            history_.pop_front();

        flushLatest(entry);
        flushHistory();

        int nch = static_cast<int>(channels.size());
        std::cout << fmt::format("EditRestore: {} – {} ch saved\n",
                                 edit_type, nch);
    }

    size_t maxHistory() const { return max_history_; }

private:
    std::string log_dir_;
    size_t max_history_;
    std::deque<nlohmann::json> history_;

    // ── Paths ────────────────────────────────────────────────────────────

    std::filesystem::path latestPath() const {
        return std::filesystem::path(log_dir_) / "last_edit_restore.json";
    }
    std::filesystem::path historyPath() const {
        return std::filesystem::path(log_dir_) / "edit_restore_history.json";
    }

    // ── Load existing history from disk on startup ───────────────────────

    void loadHistory()
    {
        auto path = historyPath();
        if (!std::filesystem::exists(path)) return;

        try {
            std::ifstream f(path);
            auto data = nlohmann::json::parse(f);
            if (data.is_array()) {
                for (auto &e : data)
                    history_.push_back(std::move(e));
                while (history_.size() > max_history_)
                    history_.pop_front();
            }
        } catch (const std::exception &e) {
            std::cerr << "SettingsEditLogger: cannot load history: "
                      << e.what() << "\n";
        }
    }

    // ── Flush latest restore point ───────────────────────────────────────

    void flushLatest(const nlohmann::json &entry)
    {
        auto path = latestPath();
        std::ofstream f(path, std::ios::trunc);
        if (!f) {
            std::cerr << "SettingsEditLogger: cannot write "
                      << path.string() << "\n";
            return;
        }
        f << entry.dump(2) << "\n";
    }

    // ── Flush rolling history ────────────────────────────────────────────

    void flushHistory()
    {
        auto path = historyPath();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto &e : history_)
            arr.push_back(e);

        std::ofstream f(path, std::ios::trunc);
        if (!f) {
            std::cerr << "SettingsEditLogger: cannot write "
                      << path.string() << "\n";
            return;
        }
        f << arr.dump(2) << "\n";
    }

    // ── Timestamp helper ─────────────────────────────────────────────────

    static std::string currentTimestamp()
    {
        auto now = std::chrono::system_clock::now();
        auto tt  = std::chrono::system_clock::to_time_t(now);
        std::tm local{};
        localtime_r(&tt, &local);

        char buf[32];
        std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
                      local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                      local.tm_hour, local.tm_min, local.tm_sec);
        return buf;
    }
};
