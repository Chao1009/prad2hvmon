#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// SettingsAutoLogger – daily auto-save of HV channel settings.
//
// Called from the HV poll loop each cycle.  On the first cycle of each
// calendar day it captures a settings snapshot and either:
//
//   • Extends the previous file's valid_date_end  (if channels unchanged)
//   • Writes a new file                           (if any param changed)
//
// Files are written to a configurable directory (default:
// database/settings_log/) with names like settings_2026-03-26.json.
// The JSON format is the standard prad2hvmon_settings_v1 plus two extra
// top-level fields:
//
//   "valid_date_start": "2026-03-20"
//   "valid_date_end":   "2026-03-26"
//
// Thread safety: tick() may be called from any single thread (the HV
// poller thread).  No internal mutex is needed because only one thread
// calls tick().
// ─────────────────────────────────────────────────────────────────────────────

#include <nlohmann/json.hpp>

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <string>
#include <vector>
#include <algorithm>

#include <fmt/format.h>

class SettingsAutoLogger
{
public:
    // log_dir: directory where daily settings files are written.
    //          Created if it doesn't exist.
    explicit SettingsAutoLogger(const std::string &log_dir = "database/settings_log")
        : log_dir_(log_dir)
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::create_directories(log_dir_, ec);
        if (ec) {
            std::cerr << "SettingsAutoLogger: cannot create directory '"
                      << log_dir_ << "': " << ec.message() << "\n";
        }
    }

    // Called every poll cycle.  Only acts on the first cycle of each new
    // calendar day.  The callback is invoked lazily — buildSnapshot() is
    // only called when a save is actually needed (at most once per day).
    //
    // Usage in HVPoller::run():
    //   settings_logger_.tick([this]() { return buildSettingsSnapshot(); });
    //
    template <typename F>
    void tick(F &&buildSnapshot)
    {
        std::string today = currentDate();
        if (today == last_checked_date_)
            return;
        last_checked_date_ = today;

        try {
            std::string snapshot_str = buildSnapshot();
            nlohmann::json snapshot = nlohmann::json::parse(snapshot_str);
            autoLog(today, snapshot);
        } catch (const std::exception &e) {
            std::cerr << "SettingsAutoLogger: " << e.what() << "\n";
        }
    }

    const std::string &logDir() const { return log_dir_; }

private:
    std::string log_dir_;
    std::string last_checked_date_;   // "YYYY-MM-DD" — prevents re-checks

    // ── Core logic ───────────────────────────────────────────────────────

    void autoLog(const std::string &today, nlohmann::json &snapshot)
    {
        namespace fs = std::filesystem;
        using json = nlohmann::json;

        auto [latest_path, latest_data] = findLatest();

        if (!latest_path.empty() && channelsMatch(latest_data, snapshot)) {
            // No change — extend the existing file's date range
            latest_data["valid_date_end"] = today;

            std::ofstream f(latest_path, std::ios::trunc);
            if (!f) {
                std::cerr << "SettingsAutoLogger: cannot write "
                          << latest_path << "\n";
                return;
            }
            f << latest_data.dump(2) << "\n";

            std::cout << fmt::format("SettingsAutoLogger: no change, "
                                     "extended {} → valid through {}\n",
                                     latest_path.filename().string(), today);
        } else {
            // Changed (or first run) — write a new file
            snapshot["valid_date_start"] = today;
            snapshot["valid_date_end"]   = today;

            std::string filename = fmt::format("settings_{}.json", today);
            fs::path path = fs::path(log_dir_) / filename;

            std::ofstream f(path, std::ios::trunc);
            if (!f) {
                std::cerr << "SettingsAutoLogger: cannot write "
                          << path.string() << "\n";
                return;
            }
            f << snapshot.dump(2) << "\n";

            int nch = snapshot.contains("channels")
                          ? static_cast<int>(snapshot["channels"].size())
                          : 0;
            std::cout << fmt::format("SettingsAutoLogger: saved {} "
                                     "({} channels)\n",
                                     filename, nch);
        }
    }

    // ── Find the most recent settings_*.json in log_dir_ ────────────────

    std::pair<std::filesystem::path, nlohmann::json> findLatest() const
    {
        namespace fs = std::filesystem;

        if (!fs::exists(log_dir_))
            return {{}, {}};

        std::vector<fs::path> files;
        for (const auto &entry : fs::directory_iterator(log_dir_)) {
            if (!entry.is_regular_file()) continue;
            auto name = entry.path().filename().string();
            // Match settings_YYYY-MM-DD.json
            if (name.rfind("settings_", 0) == 0 &&
                name.size() > 5 &&
                name.substr(name.size() - 5) == ".json")
            {
                files.push_back(entry.path());
            }
        }

        if (files.empty())
            return {{}, {}};

        // Lexicographic sort puts dates in order
        std::sort(files.begin(), files.end());
        const auto &latest = files.back();

        try {
            std::ifstream f(latest);
            nlohmann::json data = nlohmann::json::parse(f);
            return {latest, std::move(data)};
        } catch (const std::exception &e) {
            std::cerr << "SettingsAutoLogger: cannot parse "
                      << latest.string() << ": " << e.what() << "\n";
            return {{}, {}};
        }
    }

    // ── Compare channels arrays (ignoring metadata fields) ──────────────

    static bool channelsMatch(const nlohmann::json &a,
                              const nlohmann::json &b)
    {
        if (!a.contains("channels") || !b.contains("channels"))
            return false;
        return a["channels"] == b["channels"];
    }

    // ── Date helper ─────────────────────────────────────────────────────

    static std::string currentDate()
    {
        auto now = std::chrono::system_clock::now();
        auto tt  = std::chrono::system_clock::to_time_t(now);
        std::tm local{};
        localtime_r(&tt, &local);

        char buf[12];
        std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d",
                      local.tm_year + 1900, local.tm_mon + 1, local.tm_mday);
        return buf;
    }
};
