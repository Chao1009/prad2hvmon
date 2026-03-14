#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// ChannelClassifier — determines authoritative channel status on the daemon.
//
// Produces a "level" for each channel:
//   fault      — unsuppressed hardware error (OVC, OVV, TRIP, etc.)
//   suppressed — only suppressed errors present (amber, no alarm)
//   warn       — ΔV exceeds per-channel threshold (settled ON channels only)
//   ramp       — ramping up or down (RUP / RDN)
//   on         — ON, no issues
//   off        — channel is OFF
//
// ΔV thresholds are configured per channel-name pattern with wildcard
// matching (same scheme as voltage_limits.json and error_ignore.json):
//
//   database/dv_warn.json:
//   {
//     "rules": [
//       { "pattern": "W*",       "max_dv": 2.0 },
//       { "pattern": "G*",       "max_dv": 3.0 },
//       { "pattern": "PRIMARY*", "max_dv": 5.0 },
//       { "pattern": "*",        "max_dv": 2.0 }
//     ]
//   }
//
// Rules are evaluated in order — first matching pattern wins.
// If no rule matches, the channel uses a default threshold (2.0 V).
// ─────────────────────────────────────────────────────────────────────────────

#include <string>
#include <vector>
#include <cmath>
#include <algorithm>
#include <sstream>
#include <set>

struct DvWarnRule {
    std::string pattern;
    float       max_dv;
};

class ChannelClassifier
{
public:
    // ── Configuration ────────────────────────────────────────────────────

    void setDefaultDv(float dv) { default_dv_ = dv; }
    float defaultDv() const { return default_dv_; }

    void setDvRules(std::vector<DvWarnRule> rules) {
        dv_rules_ = std::move(rules);
    }

    const std::vector<DvWarnRule> &dvRules() const { return dv_rules_; }

    // ── Find the ΔV threshold for a channel by name ─────────────────────

    float dvThreshold(const std::string &ch_name) const
    {
        for (const auto &rule : dv_rules_) {
            if (matchPattern(rule.pattern, ch_name))
                return rule.max_dv;
        }
        return default_dv_;
    }

    // ── Classify a channel ───────────────────────────────────────────────
    //
    // status_str: the CAEN status string from GetStatusString()
    //             e.g. "ON", "OFF|channel is off", "ON ~OV|...", "ON OVC|..."
    // vmon, vset: voltage readback and setpoint (NAN if unavailable)
    // is_on:      power state
    // ch_name:    channel name for ΔV threshold lookup

    struct Result {
        std::string level;       // "fault", "suppressed", "warn", "ramp", "on", "off"
        bool        dv_warn;     // true if ΔV exceeds threshold
        float       dv_threshold;// the threshold used for this channel
    };

    Result classify(const std::string &status_str,
                    float vmon, float vset, bool is_on,
                    const std::string &ch_name) const
    {
        // Parse the abbreviation tokens from "TOK TOK...|detail"
        std::string abbr = status_str;
        auto pipe = abbr.find('|');
        if (pipe != std::string::npos) abbr = abbr.substr(0, pipe);

        std::vector<std::string> tokens;
        {
            std::istringstream iss(abbr);
            std::string tok;
            while (iss >> tok) tokens.push_back(tok);
        }

        // Categorise tokens
        static const std::set<std::string> normal_tokens = {
            "ON", "OFF", "RUP", "RDN"
        };

        bool has_fault      = false;
        bool has_suppressed = false;
        bool is_ramping     = false;
        bool tok_on         = false;

        for (const auto &t : tokens) {
            if (t == "RUP" || t == "RDN") { is_ramping = true; continue; }
            if (t == "ON")                { tok_on = true; continue; }
            if (t == "OFF")               { continue; }
            if (t.size() > 0 && t[0] == '~') {
                has_suppressed = true;
            } else {
                has_fault = true;
            }
        }

        // "Settled" = ON with no unsuppressed faults and not ramping
        // (suppressed errors are OK — the channel is physically running fine)
        bool is_settled = (tok_on || is_on) && !has_fault && !is_ramping;

        // ΔV check (only on settled channels with valid readings)
        float threshold = dvThreshold(ch_name);
        bool dv_warn = false;
        if (is_settled && !std::isnan(vmon) && !std::isnan(vset)) {
            dv_warn = std::fabs(vmon - vset) > threshold;
        }

        // Determine level (priority order)
        std::string level;
        if (has_fault)           level = "fault";
        else if (has_suppressed) level = "suppressed";
        else if (dv_warn)        level = "warn";
        else if (is_ramping)     level = "ramp";
        else if (tok_on || is_on)level = "on";
        else                     level = "off";

        return { level, dv_warn, threshold };
    }

private:
    // Simple wildcard matching: "W*" matches any string starting with "W",
    // "*" matches everything, "G235" is exact match.
    static bool matchPattern(const std::string &pattern, const std::string &name)
    {
        if (pattern == "*") return true;
        if (pattern.empty()) return false;
        if (pattern.back() == '*') {
            // Prefix match
            std::string prefix = pattern.substr(0, pattern.size() - 1);
            return name.size() >= prefix.size() &&
                   name.compare(0, prefix.size(), prefix) == 0;
        }
        return pattern == name;  // exact match
    }

    std::vector<DvWarnRule> dv_rules_;
    float default_dv_ = 2.0f;
};
