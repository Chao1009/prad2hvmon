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
//   Any non-empty status string that is NOT one of the normal states.
//   Normal states (not logged): "", "On", "Off", "Up", "RampUp", "RampDown"
//   (case-insensitive comparison).
// ─────────────────────────────────────────────────────────────────────────────

#include "fault_logger.h"

#include <string>
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

    // Call once per channel per poll cycle.
    void update(const std::string &name, const std::string &status)
    {
        seen_.insert(name);

        const bool was_fault = isFault(prev_status_[name]);
        const bool is_fault  = isFault(status);

        if (was_fault && prev_status_[name] != status) {
            // Old fault disappeared (or changed to a different fault/normal)
            if (logger_) logger_->log(name, prev_status_[name],
                                      FaultLogger::Direction::Disappear);
        }
        if (is_fault && prev_status_[name] != status) {
            // New fault appeared (or changed from a different fault/normal)
            if (logger_) logger_->log(name, status,
                                      FaultLogger::Direction::Appear);
        }

        prev_status_[name] = status;
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
                    logger_->log(it->first, it->second,
                                 FaultLogger::Direction::Disappear);
                }
                it = prev_status_.erase(it);
            } else {
                ++it;
            }
        }
        seen_.clear();
        if (logger_) logger_->flush();
    }

private:
    static bool isFault(const std::string &status)
    {
        if (status.empty()) return false;
        // Normalise to lower-case for comparison
        std::string s = status;
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c){ return std::tolower(c); });
        // These are normal operating states — not faults
        return s != "on" && s != "off" && s != "up"
            && s != "rampup" && s != "rampdown"
            && s != "ramp up" && s != "ramp down";
    }

    FaultLogger *logger_ = nullptr;
    std::unordered_map<std::string, std::string> prev_status_;
    std::unordered_set<std::string>              seen_;
};
