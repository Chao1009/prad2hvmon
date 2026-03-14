#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// FaultLogger – abstract interface for logging HV fault transitions.
//
// Lives in caen_lib so the polling layer can report faults without depending
// on any concrete logging framework (files, databases, Qt, etc.).
//
// A "fault transition" is a change in a channel's status between two poll
// cycles.  The logger is notified of:
//   - APPEAR  – a fault status that was not present in the previous poll
//   - DISAPPEAR – a fault status that was present but is now gone
//
// Implementations (in src/) decide what to do with these events — write to
// disk, forward to a network service, etc.
// ─────────────────────────────────────────────────────────────────────────────

#include <string>

class FaultLogger
{
public:
    enum class Direction { Appear, Disappear };

    virtual ~FaultLogger() = default;

    // Called by the polling layer when a fault transition is detected.
    //
    //   type      – "channel", "board", or "booster"
    //   name      – channel identifier (e.g. "W232" or "Booster1")
    //   status    – the fault status string (e.g. "OVC", "OVV", "TRIP",
    //               or the full status string from GetStatusString())
    //   direction – whether the fault just appeared or just disappeared
    virtual void log(const std::string &type,
                     const std::string &name,
                     const std::string &status,
                     Direction direction) = 0;

    // Optional: called once per poll cycle after all log() calls.
    // Implementations can use this to flush buffers, rotate files, etc.
    virtual void flush() {}
};
