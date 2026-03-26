# PRad-II HV Tools

Offline utilities for working with daemon log files and settings snapshots. These scripts do **not** connect to the daemon or touch hardware — they operate on local files only.

All tools are standalone Python 3 scripts with no external dependencies.

```bash
# Run from the project root (so default paths resolve)
cd /path/to/prad2hvmon
./tools/faultgrep.py [options]
./tools/merge_settings.py -c base.json new.json -o merged.json

# Or from the counting room machine
cd ~clasrun/prad2hvmon
./tools/faultgrep.py -p
```

---

## faultgrep.py

Filter and analyse fault log files. By default it reads today's log from `database/fault_log/` and prints only FAULT-level entries, stripping out the WARNs that dominate the raw files.

### Quick start

```bash
# Today's FAULTs
./tools/faultgrep.py

# Persistent FAULTs (still active — no matching DISAPPEAR)
./tools/faultgrep.py -p

# All days, persistent only
./tools/faultgrep.py -A -p

# Specific date
./tools/faultgrep.py --date 2026-03-14

# Filter by channel name (wildcards supported)
./tools/faultgrep.py -p --name 'W*'
./tools/faultgrep.py --name G232

# Explicit file(s)
./tools/faultgrep.py database/fault_log/2026-03-14.log database/fault_log/2026-03-15.log -p
```

### Modes

**Default** — prints every FAULT entry (both APPEAR and DISAPPEAR) in chronological order. Useful for reviewing the full fault history of a shift.

**Persistent (`-p`)** — tracks APPEAR/DISAPPEAR pairs by `(name, status)` key across all input files and prints only FAULTs that have no subsequent DISAPPEAR. This answers "which faults are still active?" Faults that cleared and re-appeared show the most recent APPEAR entry.

### Options

| Option | Description |
|--------|-------------|
| `-p`, `--persistent` | Show only persistent FAULTs (APPEAR without DISAPPEAR) |
| `-A`, `--all-days` | Process all `.log` files in the log directory |
| `--date YYYY-MM-DD` | Process a specific date |
| `--name PATTERN` | Filter by channel name (`*` and `?` wildcards) |
| `-d DIR`, `--dir DIR` | Fault log directory (default: `database/fault_log`) |
| `--no-color` | Disable colored output (auto-disabled when piped) |

Positional arguments are treated as explicit log file paths, bypassing `-d` / `--date` / `-A`.

### Output format

```
2026-03-14 10:23:45.123  ▶ APPEAR      channel   W232          ON OVC  VMon=1523.40  VSet=1525.00
2026-03-14 10:23:48.789  ◀ DISAPPEAR   channel   W232          ON OVC  VMon=1524.90  VSet=1525.00
```

APPEAR lines are red and DISAPPEAR lines are green when the output is a terminal. The status column shows the token half before the `|` delimiter (e.g. `ON OVC`); the full detail text (e.g. `Over Current`) is omitted for density. VMon/VSet are included when available (channel entries only).

Summary counts are printed to stderr so they don't interfere with piping:

```bash
# Pipe through grep without contaminating the filter
./tools/faultgrep.py -A | grep OVC

# Count persistent faults
./tools/faultgrep.py -A -p | wc -l
```

### How persistent matching works

The daemon's `FaultTracker` logs the same `(name, status)` pair for both APPEAR and DISAPPEAR of a given fault condition. `faultgrep -p` replays the log chronologically, tracking these pairs in a map: each APPEAR inserts an entry, each DISAPPEAR with the same key removes it. What remains at the end are faults still active when the log ends.

When processing multiple days (`-A` or multiple file arguments), files are read in filename-sorted order, so a fault that APPEARs on day 1 and DISAPPEARs on day 2 is correctly resolved.

### Examples

```bash
# Morning check: any faults still active from the overnight run?
./tools/faultgrep.py -p

# Review yesterday's full fault history
./tools/faultgrep.py --date 2026-03-21

# Find all OVC faults across the entire run period
./tools/faultgrep.py -A --name 'W*' | grep OVC

# Persistent board-level faults
./tools/faultgrep.py -A -p --name 'PRadHV*'

# Export for a shift report (no color codes)
./tools/faultgrep.py --date 2026-03-21 --no-color > shift_faults.txt
```

---

## merge_settings.py

Merge two settings files (produced by Save or `prad2hvmon read`). Starts from `<base>`, overwrites matched channels with values from `<new>`. Channels in `<base>` with no match are kept unchanged; channels only in `<new>` are ignored.

### Quick start

```bash
# By channel address — match on (crate, slot, channel), always unique
# Replaces name + params
./tools/merge_settings.py -c base.json new.json -o merged.json

# By channel name — match on name string
# Replaces params only (name unchanged), skips duplicate names
./tools/merge_settings.py -n base.json new.json -o merged.json

# Output to stdout (diagnostics go to stderr)
./tools/merge_settings.py -c base.json new.json > merged.json
```

### Modes

**By channel (`-c`)** — keys on `(crate, slot, channel)`, which is always unique. Overwrites both `name` and `params` from `<new>` into `<base>`. Params present in base but absent in new are preserved (the new params are merged on top).

**By name (`-n`)** — keys on the channel `name` string. Since names are not guaranteed unique (e.g. `SPARE`), any name that appears more than once in **either** file is skipped entirely — those channels keep their base values. Skipped names are listed at the end. Only `params` are overwritten; the name itself is unchanged.

### Options

| Option | Description |
|--------|-------------|
| `-c`, `--by-channel` | Match by (crate, slot, channel). Replaces name + params. |
| `-n`, `--by-name` | Match by channel name. Replaces params only. Skips duplicates. |
| `-o FILE`, `--output FILE` | Output file (default: stdout) |

One of `-c` or `-n` is required.

### Output

The merged JSON has the standard `prad2hvmon_settings_v1` format plus a `merged_from` field recording the source files and mode:

```json
{
  "format": "prad2hvmon_settings_v1",
  "timestamp": "2026-03-26 16:47:24",
  "merged_from": {
    "base": "base.json",
    "new": "new.json",
    "mode": "by-channel"
  },
  "channels": [ ... ]
}
```

Diagnostics are printed to stderr so stdout stays clean for piping:

```
By-channel merge: 1724 replaced, 4 unchanged, 0 in <new> with no match in <base> (ignored)
Written to merged.json (1728 channels)
```

In by-name mode, skipped names are listed at the end:

```
Skipped names (duplicate in base and/or new):
  SPARE  (duplicate in base, new)
```

### Examples

```bash
# Combine a full baseline with a file that only has updated PbWO4 voltages
./tools/merge_settings.py -n baseline.json pbwo4_update.json -o combined.json

# Merge by hardware address after a crate slot was re-cabled
./tools/merge_settings.py -c before_recable.json after_recable.json -o patched.json

# Quick diff check — merge then compare
./tools/merge_settings.py -c old.json new.json -o merged.json
diff <(jq '.channels[].params' old.json) <(jq '.channels[].params' merged.json)
```
