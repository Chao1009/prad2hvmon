# PRad-II HV Monitor

Real-time high-voltage monitoring and control for the PRad-II HyCal calorimeter (~1700 channels). A daemon (`prad2hvd`) connects to CAEN SY1527 mainframes and TDK-Lambda GEN booster supplies, polls continuously, logs faults, and serves live data to any number of browser or Qt clients via WebSocket.

## Architecture

```
┌──────────────┐     ┌──────────────┐
│ CAEN SY1527  │     │ TDK-Lambda   │
│ HV Crates    │     │ Boosters     │
└──────┬───────┘     └──────┬───────┘
       │ TCP/IP             │ SCPI/TCP
       ▼                    ▼
┌─────────────────────────────────────┐
│          prad2hvd (daemon)          │
│                                     │
│  HVPoller thread ── poll + classify │
│  BoosterPoller thread ── poll       │
│  FaultTracker ── daily log files    │
│  WebSocket + HTTP server            │
└──────────────┬──────────────────────┘
               │ http://host:8765/
               │ ws://host:8765/
       ┌───────┼───────┐
       ▼       ▼       ▼
    Browser  Qt GUI   Scripts
    client   client   (wscat)
```

The daemon owns all hardware connections and makes all status decisions (fault, warning, ΔV threshold). Clients are pure displays — they read `level`, `dv_warn`, and other fields from the daemon's JSON and render them. No classification logic runs on the client. Access control (Guest/User/Expert) is enforced server-side per WebSocket connection.

## Quick Start

```bash
# Build (daemon only, no Qt needed)
mkdir build && cd build
cmake ..
make -j$(nproc)

# Start daemon (serves both WebSocket and dashboard HTTP on port 8765)
./bin/prad2hvd

# With access control (optional — omit for full access)
./bin/prad2hvd -U operator -E expert

# Open in any browser
# http://localhost:8765/
```

### Build with Qt GUI client (optional)

```bash
cmake .. -DBUILD_GUI=ON -DCMAKE_PREFIX_PATH=/path/to/Qt5/lib/cmake
make -j$(nproc)

# Run Qt client (loads dashboard from daemon's HTTP server)
./bin/prad2hvmon -H localhost -p 8765

# Save/restore settings via daemon (no GUI needed)
./bin/prad2hvmon read -s snapshot.json
./bin/prad2hvmon write -f snapshot.json
```

## Daemon (`prad2hvd`)

Headless process that polls hardware, classifies channel status, logs faults, and broadcasts JSON snapshots over WebSocket.

| Option | Description |
|--------|-------------|
| `-c <file>` | Crates JSON (default: `database/crates.json`) |
| `-m <file>` | Module geometry JSON (default: `database/hycal_modules.json`) |
| `-g <file>` | GUI config JSON (default: `database/gui_config.json`) |
| `-d <file>` | DAQ map JSON (default: `database/daq_map.json`) |
| `-i <file>` | Error-ignore JSON (default: `database/error_ignore.json`) |
| `-l <file>` | Voltage limits JSON (default: `database/voltage_limits.json`) |
| `-w <file>` | ΔV warning rules JSON (default: `database/dv_warn.json`) |
| `-r <dir>` | Resources directory for HTTP serving (default: auto-discover) |
| `-p <port>` | WebSocket + HTTP port (default: 8765) |
| `-t <ms>` | Poll interval in ms (default: 2000) |
| `-n <count>` | Fault log buffer size for live display (default: 200) |
| `-v <level>` | Console verbosity: 0=silent, 1=faults only, 2=warn+fault (default: 2) |
| `-U <pass>` | User-level password for access control (default: none) |
| `-E <pass>` | Expert-level password for access control (default: none) |

Stop with `Ctrl+C`.

### Access Control

The daemon supports three access levels, enforced server-side. Clients authenticate via a login dialog in the dashboard header.

| Capability | Guest | User | Expert |
|---|---|---|---|
| Live monitoring (all tabs) | ✓ | ✓ | ✓ |
| Alarm mute, Save settings | ✓ | ✓ | ✓ |
| Channel ON/OFF, bulk ON/OFF | — | ✓ | ✓ |
| Booster ON/OFF | — | ✓ | ✓ |
| VSet/ISet/SVMax/Name editing | — | — | ✓ |
| Booster VSet/ISet editing | — | — | ✓ |
| Load settings, bulk Set V | — | — | ✓ |

**No passwords configured** (default): all clients get full Expert access. This is backward-compatible — the daemon behaves exactly as before.

**With passwords**: every new connection starts as Guest (watch-only). Click the access pill in the header to open the login dialog, select a level, and enter the password.

```bash
# Single password for both levels
./bin/prad2hvd -U mypassword

# Separate passwords (expert password also grants User access)
./bin/prad2hvd -U operator -E supervisor

# Only expert password set — User level is open, Expert is protected
./bin/prad2hvd -E expert123
```

The daemon logs authentication attempts to the console. All command gating is enforced server-side — client-side UI disabling is cosmetic only.

Fault logs are written to `database/fault_log/YYYY-MM-DD.log` continuously, whether or not any client is connected.

### Fault Log Format

The daily log file is tab-separated:

```
2026-03-14 10:23:45.123	FAULT	APPEAR	channel	W232	ON OVC|Over Current	1523.40	1525.00
2026-03-14 10:23:45.456	WARN	APPEAR	channel	G235	ON DVW|dV warning	1402.10	1410.00
2026-03-14 10:23:48.789	FAULT	DISAPPEAR	channel	W232	ON OVC|Over Current	1524.90	1525.00
```

Columns: timestamp, level (`FAULT`/`WARN`), direction (`APPEAR`/`DISAPPEAR`), type, name, status, VMon, VSet. The last two columns are empty for board/booster entries. The file always records both levels regardless of `-v`.

### Running as a Persistent Service

Use `tmux` to keep the daemon running after logout:

```bash
# Start in a named tmux session
tmux new-session -d -s hvd './bin/prad2hvd -v 1 -U operator -E expert'

# Detach and log out — daemon keeps running

# Reattach later to check output
tmux attach -t hvd

# Detach from session (keep daemon running): Ctrl+A then D
# (clonpc19 uses Ctrl+A as the tmux prefix instead of the default Ctrl+B)

# List sessions
tmux ls

# Stop the daemon
tmux send-keys -t hvd C-c

# Force kill if unresponsive
tmux kill-session -t hvd
```

## Qt GUI Client (`prad2hvmon`)

Optional thin client — a `QWebEngineView` window that loads `monitor.html` and connects to the daemon via WebSocket. No direct hardware access — all commands go through the daemon.

### GUI mode (default)

| Option | Description |
|--------|-------------|
| `-H <host>` | Daemon hostname (default: localhost) |
| `-p <port>` | Daemon WebSocket port (default: 8765) |
| `-r <dir>` | Resources directory (default: auto-discover) |

`Ctrl+S` saves a timestamped PNG screenshot to `database/screenshots/`.

### CLI read/write modes

Save and restore all writable channel parameters (VSet, ISet, SVMax, etc.) via the daemon — same code path as the GUI Save/Load buttons.

```bash
# Save current settings to JSON
./bin/prad2hvmon read -s snapshot.json

# Restore settings from JSON
./bin/prad2hvmon write -f snapshot.json

# Remote daemon
./bin/prad2hvmon read -H clonpc19 -s snapshot.json
./bin/prad2hvmon write -H clonpc19 -f snapshot.json
```

| Option | Description |
|--------|-------------|
| `-s <file>` | Save output path (read mode; prints to stdout if omitted) |
| `-f <file>` | Settings file to load (write mode; required) |
| `-H <host>` | Daemon hostname (default: localhost) |
| `-p <port>` | Daemon port (default: 8765) |
| `-t <sec>` | Timeout in seconds (default: 10) |

The daemon must be running. Unchanged parameters are skipped (no unnecessary hardware writes). The write command reports `restored / unchanged / skipped / errors`. Note: CLI `read` works at any access level, but CLI `write` sends `load_settings` which requires Expert. When access control is enabled (`-U`/`-E`), CLI write connects as Guest and will be rejected — temporarily omit the password flags for CLI restore operations.

## Web Client

The daemon serves the dashboard directly — open `http://<daemon-host>:8765/` in any browser. No separate file server needed. The same port handles both HTTP (for HTML/JS/CSS) and WebSocket (for live data).

Multiple clients can connect simultaneously. All receive the same live data.

### Remote Access (outside counting room)

The daemon runs on `clonpc19` behind the JLab gateway. To access it from outside:

```bash
# SSH tunnel — forward port 8765 through the gateway
ssh -L 8765:clonpc19:8765 -J your_username@hallgw.jlab.org clasrun@clonpc19
```

Then open in your local browser:

```
http://localhost:8765/
```

For the Qt GUI client:

```bash
./bin/prad2hvmon -H localhost -p 8765
```

The tunnel must stay open while you use the dashboard.

## Dashboard Features

- **Channel Table** — Sortable, filterable, live-updating. Inline VSet/ISet/SVMax/Name editing in Expert mode (click ✏ to edit, Enter/✓ to apply, Escape/✕ to cancel). Bulk ON/OFF (User+) and bulk Set V (Expert) on filtered channels. Summary strip with fault/warning counts.
- **Access Control** — Three-tier login (Guest / User / Expert) via header pill. Guest is watch-only, User enables power control, Expert unlocks all editing. Server-enforced.
- **Save / Load** — Save all writable parameters to JSON (all levels), or restore from a previously saved file (Expert only). Available from the tab bar (GUI) or command line (`prad2hvmon read`/`write`). Both paths go through the daemon — same logic, same format.
- **Board Status** — Per-board temperature, HVMax, firmware, status.
- **HyCal Geometry Map** — 2D canvas at physical positions. Color by VMon, VSet, |ΔV|, or Status. Click for draggable live popups with controls.
- **Booster HV Panel** — TDK-Lambda GEN supply cards with live readback, VSet/ISet controls, ON/OFF. Connection managed by daemon.
- **Fault Log** — Live fault transition log (configurable ring buffer, default 200 entries). Colour-coded by level (FAULT/WARN) and direction (APPEAR/DISAPPEAR). Includes VMon/VSet at the moment of transition for HV channels. Unread-indicator dot on the tab.
- **Alarm** — Audible two-tone beep every 2s on faults. Mute toggle, auto-re-arm when faults clear.

## Configuration Files

All in `database/`. The daemon reads them at startup and serves relevant ones to clients.

| File | Used by | Purpose |
|------|---------|---------|
| `crates.json` | Daemon | CAEN crate addresses `[{"name":"...","ip":"..."}]` |
| `hycal_modules.json` | Daemon + Client | Module geometry and booster definitions |
| `gui_config.json` | Client | Display thresholds, color ranges, render interval |
| `daq_map.json` | Client | DAQ readout addresses (geo tooltips) |
| `voltage_limits.json` | Daemon | Per-pattern voltage limits (OVL fault) |
| `error_ignore.json` | Daemon | Per-channel error suppression |
| `dv_warn.json` | Daemon | Per-pattern ΔV warning thresholds |

### `dv_warn.json` example

```json
{
    "default": 2.0,
    "rules": [
        { "pattern": "W*",       "max_dv": 2.0 },
        { "pattern": "G*",       "max_dv": 3.0 },
        { "pattern": "PRIMARY*", "max_dv": 5.0 },
        { "pattern": "*",        "max_dv": 2.0 }
    ]
}
```

First matching pattern wins. Same wildcard scheme as `voltage_limits.json` and `error_ignore.json`.

## Dependencies

**Daemon** (pure C++17, no Qt):
- CMake 3.14+, C++17 compiler
- `libcaenhvwrapper.so` (in `caen_lib/`)
- Auto-fetched: nlohmann/json, fmt, websocketpp, standalone Asio

**Qt GUI client** (optional):
- Qt 5 (Widgets, WebEngineWidgets, WebSockets)

## Author

Chao Peng — Argonne National Laboratory
