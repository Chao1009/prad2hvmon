#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// FaultTracker – tracks fault status across poll cycles and reports
//               transitions (appear / disappear) to a FaultLogger.
//
// Usage (in a poller's doPoll):
//
//   // After reading all channels:
//   for (auto *ch : all_channels) {
//       tracker.update(ch->GetName(), ch->GetStatusString());
//   }
//   tracker.endCycle();   // detect disappeared faults, flush logger
//
// The tracker maintains a map of { name → last_status }.  On each update():
//   - If the name is new or its status changed, log the old status as
//     DISAPPEAR (if any) and the new status as APPEAR (if non-empty).
//   - Names not seen during the cycle are logged as DISAPPEAR in endCycle().
//
// What counts as a "fault":
//   Any non-empty status string containing at least one token that is NOT
//   a normal operating state.  Normal tokens: ON, OFF, RUP, RDN.
//   CAEN format: "TOKEN TOKEN...|Detail text"  (e.g. "ON OVC|Over Current").
// ─────────────────────────────────────────────────────────────────────────────

#include "fault_logger.h"

#include <string>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>

class FaultTracker
{
public:
    // logger: the FaultLogger to report to.  Not owned; must outlive the tracker.
    //         Can be nullptr to disable logging.
    explicit FaultTracker(FaultLogger *logger = nullptr)
        : logger_(logger) {}

    void setLogger(FaultLogger *logger) { logger_ = logger; }

    // Call once per channel/board per poll cycle.
    // type:  "channel", "board", or "booster"
    // level: Fault (hardware error) or Warn (ΔV, suppressed errors, etc.)
    //        Used when a new fault/warn appears; disappear inherits the
    //        level from the original appear event.
    void update(const std::string &name, const std::string &status,
                const std::string &type = "channel",
                FaultLogger::Level level = FaultLogger::Level::Fault)
    {
        seen_.insert(name);

        const bool was_fault = isFault(prev_status_[name]);
        const bool is_fault  = isFault(status);

        if (was_fault && prev_status_[name] != status) {
            // Old fault disappeared (or changed to a different fault/normal)
            if (logger_) logger_->log(prev_type_[name], name, prev_status_[name],
                                      FaultLogger::Direction::Disappear,
                                      prev_level_[name]);
        }
        if (is_fault && prev_status_[name] != status) {
            // New fault appeared (or changed from a different fault/normal)
            if (logger_) logger_->log(type, name, status,
                                      FaultLogger::Direction::Appear, level);
        }

        prev_status_[name] = status;
        prev_type_[name]   = type;
        prev_level_[name]  = level;
    }

    // Call after all update() calls in one cycle.
    // Logs DISAPPEAR for any names that were tracked but not seen this cycle
    // (channel removed or crate disconnected), then flushes the logger.
    void endCycle()
    {
        // Find names present in prev_status_ but not in seen_
        for (auto it = prev_status_.begin(); it != prev_status_.end(); ) {
            if (seen_.find(it->first) == seen_.end()) {
                if (isFault(it->second) && logger_) {
                    logger_->log(prev_type_[it->first], it->first, it->second,
                                 FaultLogger::Direction::Disappear,
                                 prev_level_[it->first]);
                }
                prev_type_.erase(it->first);
                prev_level_.erase(it->first);
                it = prev_status_.erase(it);
            } else {
                ++it;
            }
        }
        seen_.clear();
        if (logger_) logger_->flush();
    }

private:
    // Determine whether a status string represents a fault condition.
    //
    // CAEN status format:  "TOKEN TOKEN...|Detail text"
    //   e.g. "ON", "OFF", "RUP", "RDN", "ON OVC|Over Current"
    //
    // Normal (non-fault) tokens: ON, OFF, RUP, RDN
    // Anything else (OVC, OVV, UNV, TRIP, MAXV, KILL, ILK, ...) is a fault.
    //
    // A status is a fault if it contains at least one non-normal token.
    // Empty status is not a fault.
    static bool isFault(const std::string &status)
    {
        if (status.empty()) return false;

        // Take only the abbreviation part (before '|' if present)
        std::string abbr = status;
        auto pipe = abbr.find('|');
        if (pipe != std::string::npos) abbr = abbr.substr(0, pipe);

        // Tokenise by spaces
        std::istringstream iss(abbr);
        std::string token;
        while (iss >> token) {
            // Normalise to upper-case
            std::transform(token.begin(), token.end(), token.begin(),
                           [](unsigned char c){ return std::toupper(c); });
            if (token != "ON" && token != "OFF" && token != "RUP" && token != "RDN" && token != "OK" && token != "NORMAL")
                return true;   // at least one fault token
        }
        return false;  // all tokens are normal operating states
    }

    FaultLogger *logger_ = nullptr;
    std::unordered_map<std::string, std::string> prev_status_;
    std::unordered_map<std::string, std::string> prev_type_;
    std::unordered_map<std::string, FaultLogger::Level> prev_level_;
    std::unordered_set<std::string>              seen_;
};
