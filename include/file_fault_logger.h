#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// FileFaultLogger – writes fault transitions to a timestamped log file.
//
// Log format (one line per event, tab-separated):
//
//   2026-03-14 10:23:45.123<TAB>APPEAR<TAB>channel<TAB>W232<TAB>OVC OVV
//   2026-03-14 10:23:48.456<TAB>DISAPPEAR<TAB>board<TAB>PRadHV_1_s0<TAB>OVERT|over-temperature
//
// Thread safety: all public methods are guarded by a mutex so the logger
// can be called from the HV poller thread and the booster poller thread
// concurrently (or from any other thread).
//
// File rotation: a new log file is opened each day (YYYY-MM-DD.log) in
// the configured directory.  The previous day's file is closed automatically.
// ─────────────────────────────────────────────────────────────────────────────

#include "fault_logger.h"

#include <fstream>
#include <mutex>
#include <string>

// Forward declaration — avoids circular include with hv_daemon.h
class SnapshotStore;

class FileFaultLogger : public FaultLogger
{
public:
    // Console verbosity levels (file always logs both WARN and FAULT)
    enum class Verbosity { Silent, FaultOnly, WarnAndFault };

    // log_dir: directory where log files are written.
    //          Created if it doesn't exist.
    explicit FileFaultLogger(const std::string &log_dir,
                             Verbosity verbosity = Verbosity::WarnAndFault);
    ~FileFaultLogger() override;

    void log(const std::string &type,
             const std::string &name,
             const std::string &status,
             Direction direction,
             Level level = Level::Fault,
             float vmon = NAN,
             float vset = NAN) override;

    void flush() override;

    void setVerbosity(Verbosity v) { verbosity_ = v; }

    // Set the snapshot store to push live fault log entries for WebSocket broadcast.
    // Call after constructing both the logger and the store.
    void setSnapshotStore(SnapshotStore *store) { store_ = store; }

private:
    void ensureOpen();                      // open/rotate file if needed
    static std::string today();             // "YYYY-MM-DD"
    static std::string now_timestamp();     // "YYYY-MM-DD HH:MM:SS.mmm"

    std::string   log_dir_;
    std::string   current_date_;            // date of the currently open file
    std::ofstream file_;
    std::mutex    mu_;
    Verbosity     verbosity_;
    SnapshotStore *store_ = nullptr;        // optional: push entries for WS broadcast
};
