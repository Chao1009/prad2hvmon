#!/usr/bin/env python3
# ─────────────────────────────────────────────────────────────────────────────
# faultgrep — filter and analyse prad2hvd fault logs
#
# Reads the tab-separated daily fault log files produced by the daemon
# (database/fault_log/YYYY-MM-DD.log) and prints either:
#
#   (default)      all FAULT entries, filtering out WARNs
#   --persistent   only FAULTs that APPEAR without a later DISAPPEAR
#                  (i.e. faults still active at the end of the log)
#
# Examples:
#   ./faultgrep.py                              # today's faults
#   ./faultgrep.py -p                           # today's persistent faults
#   ./faultgrep.py database/fault_log/*.log     # all days, all faults
#   ./faultgrep.py -p 2026-03-14.log 2026-03-15.log
#   ./faultgrep.py -d database/fault_log -p     # explicit log directory
#   ./faultgrep.py --name W232                  # filter by channel name
#   ./faultgrep.py --name 'W*'                  # wildcard filter
#
# Log format (tab-separated):
#   timestamp  level  direction  type  name  status  vmon  vset
#
# Author: Chao Peng — Argonne National Laboratory
# ─────────────────────────────────────────────────────────────────────────────

import argparse
import fnmatch
import glob
import os
import sys
from collections import OrderedDict
from datetime import date


# ── Column indices in the TSV ─────────────────────────────────────────────────
COL_TIME   = 0
COL_LEVEL  = 1
COL_DIR    = 2
COL_TYPE   = 3
COL_NAME   = 4
COL_STATUS = 5
COL_VMON   = 6
COL_VSET   = 7
NUM_COLS   = 8


def parse_line(line):
    """Parse a tab-separated fault log line into a dict. Returns None on failure."""
    parts = line.rstrip('\n').split('\t')
    if len(parts) < COL_STATUS + 1:
        return None
    return {
        'time':   parts[COL_TIME],
        'level':  parts[COL_LEVEL],
        'dir':    parts[COL_DIR],
        'type':   parts[COL_TYPE],
        'name':   parts[COL_NAME],
        'status': parts[COL_STATUS] if len(parts) > COL_STATUS else '',
        'vmon':   parts[COL_VMON]   if len(parts) > COL_VMON   else '',
        'vset':   parts[COL_VSET]   if len(parts) > COL_VSET   else '',
    }


def resolve_files(args):
    """Determine which log files to read, in chronological order."""
    files = []

    if args.files:
        files = args.files
    else:
        log_dir = args.dir
        if not os.path.isdir(log_dir):
            print(f"Log directory not found: {log_dir}", file=sys.stderr)
            sys.exit(1)

        if args.all_days:
            files = sorted(glob.glob(os.path.join(log_dir, '*.log')))
        elif args.date:
            path = os.path.join(log_dir, args.date + '.log')
            if os.path.isfile(path):
                files = [path]
            else:
                print(f"No log file for date: {args.date}", file=sys.stderr)
                sys.exit(1)
        else:
            # Default: today
            today = date.today().isoformat()
            path = os.path.join(log_dir, today + '.log')
            if os.path.isfile(path):
                files = [path]
            else:
                # Fall back to the most recent file
                candidates = sorted(glob.glob(os.path.join(log_dir, '*.log')))
                if candidates:
                    files = [candidates[-1]]
                    fname = os.path.basename(candidates[-1])
                    print(f"(no log for today, using most recent: {fname})",
                          file=sys.stderr)

    if not files:
        print("No fault log files found.", file=sys.stderr)
        sys.exit(1)

    return files


def name_matches(name, pattern):
    """Match channel name against a pattern (supports * and ? wildcards)."""
    if not pattern:
        return True
    return fnmatch.fnmatchcase(name, pattern)


def format_entry(e, color=True):
    """Format a single fault entry as a readable line."""
    arrow = '▶' if e['dir'] == 'APPEAR' else '◀'

    if color and sys.stdout.isatty():
        RED    = '\033[91m'
        GREEN  = '\033[92m'
        BOLD   = '\033[1m'
        DIM    = '\033[2m'
        RESET  = '\033[0m'

        dir_str = f"{RED}{arrow} {e['dir']}{RESET}" if e['dir'] == 'APPEAR' \
             else f"{GREEN}{arrow} {e['dir']}{RESET}"

        vmon_vset = ''
        if e['vmon'] or e['vset']:
            vmon_vset = f"  {DIM}VMon={e['vmon'] or '—'}  VSet={e['vset'] or '—'}{RESET}"

        status_part = e['status'].split('|')[0] if e['status'] else ''

        return (f"{DIM}{e['time']}{RESET}  "
                f"{dir_str:<22}  "
                f"{e['type']:<8}  "
                f"{BOLD}{e['name']:<12}{RESET}  "
                f"{status_part}"
                f"{vmon_vset}")
    else:
        vmon_vset = ''
        if e['vmon'] or e['vset']:
            vmon_vset = f"  VMon={e['vmon'] or '—'}  VSet={e['vset'] or '—'}"

        status_part = e['status'].split('|')[0] if e['status'] else ''

        return (f"{e['time']}  "
                f"{arrow} {e['dir']:<10}  "
                f"{e['type']:<8}  "
                f"{e['name']:<12}  "
                f"{status_part}"
                f"{vmon_vset}")


def cmd_all_faults(files, name_filter, color):
    """Print all FAULT entries (default mode)."""
    count = 0
    for path in files:
        with open(path) as f:
            for line in f:
                e = parse_line(line)
                if not e:
                    continue
                if e['level'] != 'FAULT':
                    continue
                if not name_matches(e['name'], name_filter):
                    continue
                print(format_entry(e, color))
                count += 1

    if count == 0:
        print("(no FAULT entries found)", file=sys.stderr)
    else:
        print(f"\n  {count} FAULT entries total", file=sys.stderr)


def cmd_persistent(files, name_filter, color):
    """Print only FAULTs that APPEARed without a later DISAPPEAR.

    Matching key: (name, status).  The fault tracker logs the same
    (name, status) pair for both APPEAR and DISAPPEAR of a given
    fault condition, so this is a reliable pairing.
    """
    # OrderedDict preserves first-appearance order for display
    # Key: (name, status)  →  the APPEAR entry (or None if already resolved)
    active = OrderedDict()

    for path in files:
        with open(path) as f:
            for line in f:
                e = parse_line(line)
                if not e or e['level'] != 'FAULT':
                    continue
                key = (e['name'], e['status'])
                if e['dir'] == 'APPEAR':
                    active[key] = e
                elif e['dir'] == 'DISAPPEAR':
                    active.pop(key, None)

    # Filter and display
    results = [e for e in active.values()
               if e is not None and name_matches(e['name'], name_filter)]

    if not results:
        print("(no persistent FAULTs)", file=sys.stderr)
        return

    # Group by name for readability
    by_name = OrderedDict()
    for e in results:
        by_name.setdefault(e['name'], []).append(e)

    for name, entries in by_name.items():
        for e in entries:
            print(format_entry(e, color))

    print(f"\n  {len(results)} persistent FAULTs across {len(by_name)} channels",
          file=sys.stderr)


def main():
    p = argparse.ArgumentParser(
        prog='faultgrep',
        description='Filter and analyse prad2hvd fault logs.',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""\
examples:
  %(prog)s                          today's FAULTs
  %(prog)s -p                       today's persistent FAULTs
  %(prog)s -A                       all days, all FAULTs
  %(prog)s -A -p                    all days, persistent FAULTs
  %(prog)s --date 2026-03-14        specific day
  %(prog)s --name 'W*' -p           persistent FAULTs for W-type channels
  %(prog)s path/to/2026-03-14.log   explicit file(s)
""")

    p.add_argument('files', nargs='*', default=None,
                   help='Log file(s) to read (default: today from -d)')
    p.add_argument('-d', '--dir', default='database/fault_log',
                   help='Fault log directory (default: database/fault_log)')
    p.add_argument('-p', '--persistent', action='store_true',
                   help='Show only persistent FAULTs (APPEAR without DISAPPEAR)')
    p.add_argument('-A', '--all-days', action='store_true',
                   help='Process all log files in the directory')
    p.add_argument('--date', metavar='YYYY-MM-DD',
                   help='Process a specific date')
    p.add_argument('--name', metavar='PATTERN', default=None,
                   help='Filter by channel name (supports * and ? wildcards)')
    p.add_argument('--no-color', action='store_true',
                   help='Disable colored output')

    args = p.parse_args()
    files = resolve_files(args)
    color = not args.no_color

    n_files = len(files)
    file_desc = os.path.basename(files[0]) if n_files == 1 \
           else f"{os.path.basename(files[0])} .. {os.path.basename(files[-1])}"
    print(f"Reading {n_files} file(s): {file_desc}", file=sys.stderr)

    if args.persistent:
        cmd_persistent(files, args.name, color)
    else:
        cmd_all_faults(files, args.name, color)


if __name__ == '__main__':
    main()
