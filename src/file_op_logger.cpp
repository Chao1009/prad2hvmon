// ─────────────────────────────────────────────────────────────────────────────
// FileOpLogger implementation
// ─────────────────────────────────────────────────────────────────────────────

#include "file_op_logger.h"

#include <chrono>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <sstream>

namespace fs = std::filesystem;
using json = nlohmann::json;

FileOpLogger::FileOpLogger(const std::string &log_dir)
    : log_dir_(log_dir)
{
    std::error_code ec;
    fs::create_directories(log_dir_, ec);
    if (ec) {
        std::cerr << "FileOpLogger: cannot create log directory '"
                  << log_dir_ << "': " << ec.message() << "\n";
    }
}

FileOpLogger::~FileOpLogger()
{
    std::lock_guard<std::mutex> lk(mu_);
    if (file_.is_open()) file_.close();
}

void FileOpLogger::log(const std::string &access_level,
                       const std::string &op_type,
                       const std::string &detail)
{
    std::lock_guard<std::mutex> lk(mu_);
    ensureOpen();

    std::string timestamp = now_timestamp();

    if (file_.is_open()) {
        file_ << timestamp    << '\t'
              << access_level << '\t'
              << op_type      << '\t'
              << detail       << '\n';
        file_.flush();
    }

    // Also print to console
    std::cout << "OP  " << access_level << "  " << op_type << "  " << detail << "\n";
}

void FileOpLogger::logCommand(int access_level, const json &cmd)
{
    std::string type = cmd.value("type", "");
    if (type.empty()) return;

    log(levelLabel(access_level), type, formatDetail(cmd));
}

void FileOpLogger::logAuth(int requested, int granted, const std::string &reason)
{
    std::string detail;
    if (granted == requested) {
        detail = "granted";
    } else {
        detail = "denied";
        if (!reason.empty())
            detail += " (" + reason + ")";
    }

    // Show the transition: "Guest→Expert" or "User→Guest"
    std::string level_str = std::string(levelLabel(requested));
    if (granted != requested)
        level_str += std::string("→") + levelLabel(granted);

    log(level_str, "auth", detail);
}

void FileOpLogger::flush()
{
    std::lock_guard<std::mutex> lk(mu_);
    if (file_.is_open()) file_.flush();
}

// ── Private helpers ──────────────────────────────────────────────────────────

const char *FileOpLogger::levelLabel(int lvl)
{
    switch (lvl) {
    case 0:  return "Guest";
    case 1:  return "User";
    case 2:  return "Expert";
    default: return "Unknown";
    }
}

std::string FileOpLogger::formatDetail(const json &cmd)
{
    std::string type = cmd.value("type", "");
    std::ostringstream ss;

    // Channel-targeted commands: crate/slot/ch + type-specific params
    if (type == "set_power" || type == "set_voltage" || type == "set_current" ||
        type == "set_svmax" || type == "set_name")
    {
        ss << "crate=" << cmd.value("crate", "?")
           << " slot=" << cmd.value("slot", -1)
           << " ch="   << cmd.value("ch", -1);

        if (type == "set_power")
            ss << " on=" << (cmd.value("on", false) ? "true" : "false");
        else if (type == "set_name")
            ss << " name=\"" << cmd.value("name", "") << "\"";
        else
            ss << " value=" << cmd.value("value", 0.0);
    }
    // Bulk HV commands
    else if (type == "set_all_power") {
        ss << "on=" << (cmd.value("on", false) ? "true" : "false");
    }
    else if (type == "set_power_batch") {
        ss << "on=" << (cmd.value("on", false) ? "true" : "false");
        if (cmd.contains("channels") && cmd["channels"].is_array())
            ss << " channels=" << cmd["channels"].size();
        if (cmd.contains("filter") && cmd["filter"].is_object()) {
            const auto &f = cmd["filter"];
            ss << " group=" << f.value("group", "all")
               << " crate=" << f.value("crate", "all");
            std::string search = f.value("search", "");
            if (!search.empty())
                ss << " search=\"" << search << "\"";
        }
    }
    else if (type == "set_all_voltage") {
        ss << "value=" << cmd.value("value", 0.0);
    }
    // Booster commands
    else if (type == "booster_set_output") {
        ss << "idx=" << cmd.value("idx", -1)
           << " on=" << (cmd.value("on", false) ? "true" : "false");
    }
    else if (type == "booster_set_voltage" || type == "booster_set_current") {
        ss << "idx=" << cmd.value("idx", -1)
           << " value=" << cmd.value("value", 0.0);
    }
    // Save/load
    else if (type == "save_settings") {
        ss << "snapshot requested";
    }
    else if (type == "load_settings") {
        // Count channels in the settings payload if present
        if (cmd.contains("settings") && cmd["settings"].contains("channels")) {
            ss << "channels=" << cmd["settings"]["channels"].size();
        } else {
            ss << "settings provided";
        }
    }
    // Fallback
    else {
        ss << cmd.dump();
    }

    return ss.str();
}

void FileOpLogger::ensureOpen()
{
    // Already holding mu_
    const std::string d = today();
    if (file_.is_open() && d == current_date_) return;

    // Date changed (or first call) — rotate
    if (file_.is_open()) file_.close();

    const std::string path = log_dir_ + "/" + d + ".log";
    file_.open(path, std::ios::app);
    if (!file_.is_open()) {
        std::cerr << "FileOpLogger: cannot open '" << path << "'\n";
        return;
    }
    current_date_ = d;
}

std::string FileOpLogger::today()
{
    const auto now  = std::chrono::system_clock::now();
    const auto tt   = std::chrono::system_clock::to_time_t(now);
    std::tm    local{};
    localtime_r(&tt, &local);

    char buf[12];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d",
                  local.tm_year + 1900, local.tm_mon + 1, local.tm_mday);
    return buf;
}

std::string FileOpLogger::now_timestamp()
{
    const auto now   = std::chrono::system_clock::now();
    const auto tt    = std::chrono::system_clock::to_time_t(now);
    const auto ms    = std::chrono::duration_cast<std::chrono::milliseconds>(
                           now.time_since_epoch()) % 1000;
    std::tm    local{};
    localtime_r(&tt, &local);

    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d.%03d",
                  local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                  local.tm_hour, local.tm_min, local.tm_sec,
                  static_cast<int>(ms.count()));
    return buf;
}
