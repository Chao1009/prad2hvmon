# PRad-II HV Monitor

Real-time high-voltage monitoring and control for the PRad-II HyCal calorimeter (~1200 channels). Communicates with CAEN SY1527 mainframes (TCP/IP) and TDK-Lambda GEN booster supplies (SCPI/TCP). Dashboard served via Qt WebEngine.

## Modes

| Mode | Usage | Description |
|------|-------|-------------|
| `gui` | `./prad2hvmon [gui]` | Interactive dashboard (default) |
| `read` | `./prad2hvmon read [-s file.json]` | Save all writable params to JSON |
| `write` | `./prad2hvmon write -f file.json` | Restore writable params from JSON |
| `convert` | `./prad2hvmon convert -f old.txt -s new.json` | Convert old text format → JSON |
| `hv_params` | `./prad2hvmon hv_params` | Print discovered board/channel param info |

### Common Options

| Option | Description |
|--------|-------------|
| `-c <file>` | Crates JSON config (default: auto-discover) |
| `-f <file>` | Input settings file (write/convert modes) |
| `-s <file>` | Output file (read/convert modes) |
| `-l <file>` | Voltage limits JSON (default: auto-discover) |
| `-i <file>` | Error-ignore JSON (default: auto-discover) |
| `-m <file>` | Module geometry JSON (GUI mode) |

## Quick Start (Hall B)

```bash
ssh clasrun@clonpc19
cd ~/prad2_daq/prad2hvmon/build
./bin/prad2hvmon                          # GUI mode
./bin/prad2hvmon read -s snapshot.json    # save settings
./bin/prad2hvmon write -f snapshot.json   # restore settings
```

## Building

Requires C++17, CMake 3.11+, Qt 5 (Core, Widgets, WebEngineWidgets, WebChannel), CAENHVWrapper (`libcaenhvwrapper.so` in `caen_lib/`), and fmt (auto-fetched).

```bash
mkdir build && cd build
cmake .. -DCMAKE_PREFIX_PATH=/path/to/Qt5/lib/cmake
make -j$(nproc)
```

## GUI Features

**Channel Table** — Live-updating sortable table with VMon, VSet, ΔV, IMon, ISet, SVMax, status. Summary strip with filter chips (Total/Primary/ON/OFF/Warn/Fault). Inline VSet editing, power toggle, bulk ON/OFF. Expert mode gates write operations.

**HyCal Geometry Map** — 2D canvas of all detector modules at physical positions. Color by VMon, VSet, |ΔV|, or Status. Click to open draggable live-update popups. Tooltip shows VMon/VSet, IMon/ISet, DAQ info, status.

**Board Status Tab** — Per-board info: model, serial, firmware, HVMax, temperature (color-coded), status. Red dot on tab when any board has faults.

**Booster HV Panel** — TDK-Lambda GEN supplies via SCPI/TCP. Manual connect (avoids locking out other instances). Per-supply cards with VSet/ISet controls, ON/OFF, CV/CC mode display. Retry/Disconnect buttons in header.

**Alarm & Fault Logger** — Audible two-tone beep on faults, mute toggle, auto-re-arm. Daily rotating fault log files in `database/fault_log/`.

**Screenshots** — Press Ctrl+S to save a timestamped PNG.

## Settings File Format (JSON)

The `read` and `write` modes use a JSON format that captures all writable parameters discovered from the hardware:

```json
{
    "format": "prad2hvmon_settings_v1",
    "timestamp": "2026-03-14 15:30:42",
    "channels": [
        {
            "crate": "PRadHV_1",
            "slot": 0,
            "channel": 1,
            "name": "G235",
            "params": {
                "V0Set": 1500.0,
                "I0Set": 25.0,
                "SVMax": 2000.0,
                "RUp": 50.0,
                "RDWn": 50.0,
                "Pw": 1,
                "PDwn": 0
            }
        }
    ]
}
```

The `params` object includes only writable parameters — which ones appear depends on the board model (auto-discovered at init). Monitor-only params (VMon, IMon, Status) are not saved.

The `write` command validates each param against the board's discovered capabilities before restoring. Params that don't exist on the actual hardware are skipped with a warning.

Use `convert` to migrate old whitespace-format settings files (only V0Set will be populated, since the old format only stored VSet).

## Generic Parameter System

Parameters are auto-discovered from each CAEN board at init time via `CAENHV_GetChParamInfo`/`GetBdParamInfo`. No hard-coded parameter names in the data model — the system adapts to any board type. Use `hv_params` mode to inspect what each board supports:

```
═══ PRadHV_1 slot 0 — Model A1932 (49 ch, serial 3, fw 258) ═══

  Board parameters:
    BdStatus          BDSTATUS    RD
    Temp              NUMERIC     RD  [5.0 .. 65.0] °C

  Channel parameters (ch 0):
    V0Set             NUMERIC     RW  [0.0 .. 3100.0] V
    I0Set             NUMERIC     RW  [0.0 .. 30.0] mA
    V1Set             NUMERIC     RW  [0.0 .. 3100.0] V
    I1Set             NUMERIC     RW  [0.0 .. 30.0] mA
    RUp               NUMERIC     RW  [1.0 .. 500.0] V/s
    RDWn              NUMERIC     RW  [1.0 .. 500.0] V/s
    SVMax             NUMERIC     RW  [0.0 .. 3100.0] V
    VMon              NUMERIC     RD  [0.0 .. 3100.0] V
    IMon              NUMERIC     RD  [0.0 .. 30.0] mA
    Status            CHSTATUS    RD
    Pw                ONOFF       RW
    PDwn              ONOFF       RW
```

Polling reads all discovered params generically. When a bulk read fails (e.g. PRIMARY channel doesn't support SVMax), it probes each channel once to build a fallback list, then uses a reduced bulk read on subsequent cycles.

## Configuration Files

All config files live in `database/`. The application auto-discovers them relative to the binary.

**`crates.json`** — CAEN SY1527 addresses. Array of `{"name": "...", "ip": "..."}`.

**`gui_config.json`** — Window size, ΔV thresholds, color ranges, poll/render intervals.

**`hycal_modules.json`** — Module geometry (`"t": "PbGlass"/"PbWO4"/"LMS"` with x/y/sx/sy) and booster definitions (`"t": "booster"` with ip/port).

**`daq_map.json`** — Optional. Maps module names to DAQ readout addresses (shown in geo tooltips).

**`voltage_limits.json`** — Optional. Pattern-based voltage limit overrides. First match wins. Supports trailing wildcards (`"G*"`, `"*"`). When VMon exceeds the limit on an active channel, an `OVL` software fault is raised.

**`error_ignore.json`** — Optional. List of channel names whose status errors are suppressed from stderr.

## Channel Types & Default Limits

| Pattern | Type | Limit | Description |
|---------|------|-------|-------------|
| `G*` | PbGlass | 1950 V | Lead glass modules |
| `W*` | PbWO4 | 1450 V | Lead tungstate crystals |
| `PRIMARY*` | Primary | 3000 V | Board-level control (ch 0, A1932) |
| `L*` | LMS | 2000 V | Light monitoring PMTs |
| `S*` | Scintillator | 2000 V | Scintillator counters |
| `H*` | Veto | 2000 V | PrimEx veto counters |

Override via `database/voltage_limits.json`. Unrecognised prefixes default to 1500 V.

## Architecture

```
CAEN SY1527 ◄── TCP/IP ──► HVPoller (worker thread)
                                │ ReadAllParams() → generic param map
                                │ FaultTracker → FileFaultLogger
                                ▼
                           HVMonitor (GUI thread, QWebChannel)
                                │ snapshotReady / boardSnapshotReady
                                ▼
TDK-Lambda GEN ◄─ SCPI/TCP ─► BoosterPoller (worker thread)
                                │
                           BoosterMonitor (GUI thread, QWebChannel)
                                │ boosterUpdated
                                ▼
                           monitor.html/js/css (Qt WebEngine)
```

HVPoller owns all CAEN objects on a dedicated thread. HVMonitor bridges to the JS frontend via QWebChannel with cached JSON snapshots. The JS frontend runs a fast render loop (200ms) independently of the poll cadence.

## Author

Chao Peng — Argonne National Laboratory
