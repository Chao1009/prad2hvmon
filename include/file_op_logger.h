#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// FileOpLogger – writes user operations to a timestamped log file.
//
// Log format (one line per event, tab-separated):
//
//   2026-03-14 10:23:45.123<TAB>Expert<TAB>set_voltage<TAB>crate=PRadHV slot=0 ch=32 value=1525.00
//   2026-03-14 10:23:46.789<TAB>User<TAB>set_power<TAB>crate=PRadHV slot=0 ch=32 on=true
//   2026-03-14 10:23:48.100<TAB>Guest→Expert<TAB>auth<TAB>granted
//
// Thread safety: all public methods are guarded by a mutex so the logger
// can be called from the WebSocket server thread safely.
//
// File rotation: a new log file is opened each day (YYYY-MM-DD.log) in
// the configured directory.  The previous day's file is closed automatically.
// ─────────────────────────────────────────────────────────────────────────────

#include <nlohmann/json.hpp>

#include <fstream>
#include <mutex>
#include <string>

class FileOpLogger
{
public:
    // log_dir: directory where log files are written.
    //          Created if it doesn't exist.
    explicit FileOpLogger(const std::string &log_dir);
    ~FileOpLogger();

    // Log a user operation.
    //   access_level: the client's access level string ("Guest", "User", "Expert")
    //   op_type:      the command type ("set_voltage", "auth", etc.)
    //   detail:       human-readable summary of the operation parameters
    void log(const std::string &access_level,
             const std::string &op_type,
             const std::string &detail);

    // Convenience: extract details from a JSON command and log it.
    // access_level is the numeric level (0/1/2 → Guest/User/Expert).
    void logCommand(int access_level, const nlohmann::json &cmd);

    // Log an authentication event.
    void logAuth(int requested, int granted, const std::string &reason = "");

    void flush();

private:
    void ensureOpen();                      // open/rotate file if needed
    static std::string today();             // "YYYY-MM-DD"
    static std::string now_timestamp();     // "YYYY-MM-DD HH:MM:SS.mmm"
    static const char *levelLabel(int lvl); // 0→"Guest", 1→"User", 2→"Expert"

    // Build a human-readable detail string from a command JSON
    static std::string formatDetail(const nlohmann::json &cmd);

    std::string   log_dir_;
    std::string   current_date_;            // date of the currently open file
    std::ofstream file_;
    std::mutex    mu_;
};
