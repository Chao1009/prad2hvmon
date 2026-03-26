# PRad-II HV Tools

Offline utilities for fault logs, settings snapshots, config files, and hardware operations. Standalone Python 3, no external dependencies, no daemon connection.

```bash
cd /path/to/prad2hvmon        # or ~clasrun/prad2hvmon
./tools/faultgrep.py -p
./tools/merge_settings.py -c base.json new.json -o merged.json
./tools/json2table.py database/daq_map.json
./tools/caenhv_reset.py --soft hallb-hv1
```

---

## faultgrep.py

Filter fault log files. Default: today's FAULTs from `database/fault_log/`.

```bash
./tools/faultgrep.py                          # today's FAULTs
./tools/faultgrep.py -p                       # persistent only (no matching DISAPPEAR)
./tools/faultgrep.py -A -p                    # all days, persistent only
./tools/faultgrep.py --date 2026-03-14        # specific date
./tools/faultgrep.py -p --name 'W*'           # filter by channel (wildcards)
./tools/faultgrep.py -A | grep OVC            # pipe-friendly (summary → stderr)
```

| Option | Description |
|--------|-------------|
| `-p`, `--persistent` | Only FAULTs with no subsequent DISAPPEAR |
| `-A`, `--all-days` | All `.log` files in the log directory |
| `--date YYYY-MM-DD` | Specific date |
| `--name PATTERN` | Filter by name (`*` `?` wildcards) |
| `-d DIR` | Log directory (default: `database/fault_log`) |
| `--no-color` | Plain output (auto when piped) |

Positional args are treated as explicit file paths. Persistent mode replays APPEAR/DISAPPEAR pairs chronologically; files sorted by name so multi-day resolution works.

---

## merge_settings.py

Merge two settings files. Base channels with no match in new are kept; channels only in new are ignored.

```bash
# By address (crate,slot,ch) — replaces name + params
./tools/merge_settings.py -c base.json new.json -o merged.json

# By name — replaces params only, skips duplicate names
./tools/merge_settings.py -n base.json new.json -o merged.json

# Stdout (diagnostics → stderr)
./tools/merge_settings.py -c base.json new.json > merged.json
```

| Option | Description |
|--------|-------------|
| `-c`, `--by-channel` | Match by (crate, slot, channel). Replaces name + params. |
| `-n`, `--by-name` | Match by name. Replaces params only. Skips duplicates. |
| `-o FILE` | Output file (default: stdout) |

One of `-c` or `-n` is required. Output is standard `prad2hvmon_settings_v1` JSON with an added `merged_from` field.

---

## json2table.py

Pretty-print JSON config/settings files as aligned text tables. Auto-detects file type from content.

```bash
# Config files
./tools/json2table.py database/daq_map.json
./tools/json2table.py database/hycal_modules.json

# Settings snapshot (shows address, name, and all writable params)
./tools/json2table.py snapshot.json

# Filter + sort
./tools/json2table.py snapshot.json -f "crate=PRadHV_1" -s V0Set
./tools/json2table.py snapshot.json -f "name~W" -f "crate=PRadHV_2" -s name
./tools/json2table.py database/daq_map.json -f "name~G" -s slot

# CSV export
./tools/json2table.py snapshot.json --csv > channels.csv
```

| Option | Description |
|--------|-------------|
| `-s COL` | Sort by column name |
| `-f EXPR` | Filter: `COL=VAL` (exact) or `COL~PAT` (substring). Repeatable. |
| `-t TYPE` | Force type: `daq`, `modules`, `settings` (default: auto) |
| `-c`, `--csv` | Output as CSV |
| `-n`, `--no-header` | Suppress file-info header line |

Supported files: `daq_map.json` (name/crate/slot/ch table), `hycal_modules.json` (detector modules and boosters as separate blocks), and settings snapshots (address + name + param columns). Numbers right-align automatically.

---

## caenhv_reset.py

Remote reboot of CAEN HV mainframes via the tsconnect serial port. Requires the JLab CLON environment (`$CLON_PARMS`, `$EPICS`) and SSH access to `clon00`. **Not part of the daemon** — this is a standalone ops tool.

Original author: Nathan Baltzell (Jefferson Lab). Modernised for Python 3.

```bash
# CPU reboot (should not affect voltages)
./tools/caenhv_reset.py --soft hallb-hv1

# Full power cycle (brings down ALL voltages — asks for confirmation)
./tools/caenhv_reset.py --hard hallb-hv1
```

| Option | Description |
|--------|-------------|
| `--soft` | CPU reboot only — should not affect running voltages |
| `--hard` | Full power cycle — **brings down all voltages** (requires confirmation) |
| `mainframe` | Hostname of the CAEN mainframe (positional) |

The script looks up the serial device in `$CLON_PARMS/tsconnect/tsconnect.conf` and SSHs to `clon00` to run the reset binary. Hard reset prompts for confirmation before proceeding.
