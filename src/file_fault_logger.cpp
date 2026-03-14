// ─────────────────────────────────────────────────────────────────────────────
// FileFaultLogger implementation
// ─────────────────────────────────────────────────────────────────────────────

#include "file_fault_logger.h"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <filesystem>

namespace fs = std::filesystem;

FileFaultLogger::FileFaultLogger(const std::string &log_dir)
    : log_dir_(log_dir)
{
    // Ensure the log directory exists
    std::error_code ec;
    fs::create_directories(log_dir_, ec);
    if (ec) {
        std::cerr << "FileFaultLogger: cannot create log directory '"
                  << log_dir_ << "': " << ec.message() << "\n";
    }
}

FileFaultLogger::~FileFaultLogger()
{
    std::lock_guard<std::mutex> lk(mu_);
    if (file_.is_open()) file_.close();
}

void FileFaultLogger::log(const std::string &name,
                          const std::string &status,
                          Direction direction)
{
    std::lock_guard<std::mutex> lk(mu_);
    ensureOpen();
    if (!file_.is_open()) return;

    const char *dir_str = (direction == Direction::Appear) ? "APPEAR"
                                                           : "DISAPPEAR";
    file_ << now_timestamp() << '\t'
          << dir_str         << '\t'
          << name            << '\t'
          << status          << '\n';
}

void FileFaultLogger::flush()
{
    std::lock_guard<std::mutex> lk(mu_);
    if (file_.is_open()) file_.flush();
}

// ── Private helpers ──────────────────────────────────────────────────────────

void FileFaultLogger::ensureOpen()
{
    // Already holding mu_
    const std::string d = today();
    if (file_.is_open() && d == current_date_) return;

    // Date changed (or first call) — rotate
    if (file_.is_open()) file_.close();

    const std::string path = log_dir_ + "/" + d + ".log";
    file_.open(path, std::ios::app);
    if (!file_.is_open()) {
        std::cerr << "FileFaultLogger: cannot open '" << path << "'\n";
        return;
    }
    current_date_ = d;
}

std::string FileFaultLogger::today()
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

std::string FileFaultLogger::now_timestamp()
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
