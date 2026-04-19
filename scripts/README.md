# PRad-II HV Scripts

This directory holds two things:

- **Environment scripts** — `setup.sh` / `setup.csh.in` are installed into `<prefix>/bin/`. Source them to put `prad2hvd`, `prad2hvmon`, the Python tool wrappers, and `libcaenhvwrapper.so` on your `PATH` / `LD_LIBRARY_PATH` and export `PRAD2HV_DATABASE_DIR` / `PRAD2HV_RESOURCE_DIR`. You normally don't run them from here.
- **Offline Python utilities** — fault-log filtering, settings-snapshot manipulation, config-file printing, and remote CAEN reset. Standalone Python 3, no daemon connection. Each tool also gets a thin `bin/` wrapper at install time, so after `make install` you can run `faultgrep`, `merge_settings`, `json2table`, `caenhv_reset`, `vmon_reader` directly without sourcing `setup.sh`.

```bash
cd /path/to/prad2hvmon        # or ~clasrun/prad2hvmon
./scripts/faultgrep.py -p
./scripts/merge_settings.py -c base.json new.json -o merged.json
./scripts/json2table.py database/hycal_modules.json
./scripts/caenhv_reset.py --soft hallb-hv1
```

---

## faultgrep.py

Filter fault log files. Default log dir: `$PRAD2HV_DATABASE_DIR/fault_log` if set, else `database/fault_log`. If the daemon is writing logs elsewhere (via `prad2hvd -d <dir>`), point `faultgrep.py -d <dir>/fault_log` at that location.

```bash
./scripts/faultgrep.py                          # today's FAULTs
./scripts/faultgrep.py -p                       # persistent only (no matching DISAPPEAR)
./scripts/faultgrep.py -A -p                    # all days, persistent only
./scripts/faultgrep.py --date 2026-03-14        # specific date
./scripts/faultgrep.py -p --name 'W*'           # filter by channel (wildcards)
./scripts/faultgrep.py -A | grep OVC            # pipe-friendly (summary → stderr)
```

| Option | Description |
|--------|-------------|
| `-p`, `--persistent` | Only FAULTs with no subsequent DISAPPEAR |
| `-A`, `--all-days` | All `.log` files in the log directory |
| `--date YYYY-MM-DD` | Specific date |
| `--name PATTERN` | Filter by name (`*` `?` wildcards) |
| `-d DIR` | Log directory (default: `$PRAD2HV_DATABASE_DIR/fault_log` if set, else `database/fault_log`) |
| `--no-color` | Plain output (auto when piped) |

Positional args are treated as explicit file paths. Persistent mode replays APPEAR/DISAPPEAR pairs chronologically; files sorted by name so multi-day resolution works.

---

## merge_settings.py

Merge two settings files. Base channels with no match in new are kept; channels only in new are ignored.

```bash
# By address (crate,slot,ch) — replaces name + params
./scripts/merge_settings.py -c base.json new.json -o merged.json

# By name — replaces params only, skips duplicate names
./scripts/merge_settings.py -n base.json new.json -o merged.json

# Stdout (diagnostics → stderr)
./scripts/merge_settings.py -c base.json new.json > merged.json
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
./scripts/json2table.py database/hycal_modules.json

# Settings snapshot (shows address, name, and all writable params)
./scripts/json2table.py snapshot.json

# Filter + sort
./scripts/json2table.py snapshot.json -f "crate=PRadHV_1" -s V0Set
./scripts/json2table.py snapshot.json -f "name~W" -f "crate=PRadHV_2" -s name

# CSV export
./scripts/json2table.py snapshot.json --csv > channels.csv
```

| Option | Description |
|--------|-------------|
| `-s COL` | Sort by column name |
| `-f EXPR` | Filter: `COL=VAL` (exact) or `COL~PAT` (substring). Repeatable. |
| `-t TYPE` | Force type: `modules`, `settings` (default: auto) |
| `-c`, `--csv` | Output as CSV |
| `-n`, `--no-header` | Suppress file-info header line |

Supported files: `hycal_modules.json` (detector modules and boosters as separate blocks), and settings snapshots (address + name + param columns). Numbers right-align automatically.

---

## caenhv_reset.py

Remote reboot of CAEN HV mainframes via the tsconnect serial port. Requires the JLab CLON environment (`$CLON_PARMS`, `$EPICS`) and SSH access to `clon00`. **Not part of the daemon** — this is a standalone ops tool.

Original author: Nathan Baltzell (Jefferson Lab). Modernised for Python 3.

```bash
# CPU reboot (should not affect voltages)
./scripts/caenhv_reset.py --soft hallb-hv1

# Full power cycle (brings down ALL voltages — asks for confirmation)
./scripts/caenhv_reset.py --hard hallb-hv1
```

| Option | Description |
|--------|-------------|
| `--soft` | CPU reboot only — should not affect running voltages |
| `--hard` | Full power cycle — **brings down all voltages** (requires confirmation) |
| `mainframe` | Hostname of the CAEN mainframe (positional) |

The script looks up the serial device in `$CLON_PARMS/tsconnect/tsconnect.conf` and SSHs to `clon00` to run the reset binary. Hard reset prompts for confirmation before proceeding.
