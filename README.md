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

The daemon owns all hardware connections and makes all status decisions (fault, warning, ΔV threshold). Clients are pure displays — they read `level`, `dv_warn`, and other fields from the daemon's JSON and render them. No classification logic runs on the client.

## Quick Start

```bash
# Build (daemon only, no Qt needed)
mkdir build && cd build
cmake ..
make -j$(nproc)

# Start daemon (serves both WebSocket and dashboard HTTP on port 8765)
./bin/prad2hvd

# Open in any browser
# http://clonpc19:8765/
```

### Build with Qt GUI client (optional)

```bash
cmake .. -DBUILD_GUI=ON -DCMAKE_PREFIX_PATH=/path/to/Qt5/lib/cmake
make -j$(nproc)

# Run Qt client (loads dashboard from daemon's HTTP server)
./bin/prad2hvmon -H clonpc19 -p 8765
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
| `-t <ms>` | Poll interval in ms (default: 3000) |

Stop with `Ctrl+C`. Fault logs are written to `database/fault_log/YYYY-MM-DD.log` continuously, whether or not any client is connected.

## Qt GUI Client (`prad2hvmon`)

Optional thin client — a `QWebEngineView` window that loads `monitor.html` and connects to the daemon via WebSocket. No hardware access, no QWebChannel.

| Option | Description |
|--------|-------------|
| `-H <host>` | Daemon hostname (default: localhost) |
| `-p <port>` | Daemon WebSocket port (default: 8765) |
| `-r <dir>` | Resources directory (default: auto-discover) |
| `--width <px>` | Window width (default: 1400) |
| `--height <px>` | Window height (default: 900) |

`Ctrl+S` saves a timestamped PNG screenshot.

## Web Client

The daemon serves the dashboard directly — open `http://<daemon-host>:8765/` in any browser. No separate file server needed. The same port handles both HTTP (for HTML/JS/CSS) and WebSocket (for live data).

Multiple clients can connect simultaneously. All receive the same live data.

### Remote Access (outside counting room)

The daemon runs on `clonpc19` behind the JLab gateway `hallgw`. To access it from outside:

```bash
# SSH tunnel — forward port 8765 through the gateway
ssh -L 8765:clonpc19:8765 -J your_username@hallgw clasrun@clonpc19
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

- **Channel Table** — Sortable, filterable, live-updating. Inline VSet/ISet/SVMax/Name editing in expert mode (apply button appears only when value changes; Enter or click to apply). Bulk ON/OFF. Summary strip with fault/warning counts.
- **Board Status** — Per-board temperature, HVMax, firmware, status.
- **HyCal Geometry Map** — 2D canvas at physical positions. Color by VMon, VSet, |ΔV|, or Status. Click for draggable live popups with controls.
- **Booster HV Panel** — TDK-Lambda GEN supply cards with readback, VSet/ISet controls, ON/OFF. Connect/Disconnect/Retry buttons.
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
- Qt 5 (Widgets, WebEngineWidgets)

## Author

Chao Peng — Argonne National Laboratory
