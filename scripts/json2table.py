#!/usr/bin/env python3
# ─────────────────────────────────────────────────────────────────────────────
# json2table.py — Pretty-print PRad-II JSON config files as text tables
#
# Supports:
#   hycal_modules.json  — Module geometry + booster definitions
#   settings JSON       — Saved HV channel parameters (prad2hvmon read output)
#
# The file type is auto-detected from content.
#
# Usage:
#   python3 json2table.py <file.json> [options]
#
# Options:
#   -s, --sort COL      Sort by column name (case-insensitive)
#   -f, --filter EXPR   Filter rows: COL=VAL or COL~PATTERN (substring match)
#   -t, --type TYPE     Force type: modules, settings (auto-detected)
#   -c, --csv           Output as CSV instead of table
#   -n, --no-header     Suppress the file-info header
#   -h, --help          Show help
#
# Author: Chao Peng — Argonne National Laboratory
# ─────────────────────────────────────────────────────────────────────────────

import json
import sys
import argparse
import os
from collections import OrderedDict


# ═════════════════════════════════════════════════════════════════════════════
#  Table formatter
# ═════════════════════════════════════════════════════════════════════════════

def format_table(headers, rows, align=None):
    """
    Format a list of rows into an aligned text table.
    align: list of 'l' or 'r' per column (default: 'l' for strings, 'r' for numbers)
    """
    if not rows:
        return "(no data)\n"

    # Convert everything to strings
    str_rows = []
    for row in rows:
        str_rows.append([str(v) if v is not None else '' for v in row])

    # Compute column widths
    widths = [len(h) for h in headers]
    for row in str_rows:
        for i, cell in enumerate(row):
            if i < len(widths):
                widths[i] = max(widths[i], len(cell))

    # Auto-detect alignment if not given
    if align is None:
        align = []
        for i in range(len(headers)):
            # Check if the column is numeric
            is_num = True
            for row in str_rows:
                if i < len(row) and row[i] != '':
                    try:
                        float(row[i])
                    except ValueError:
                        is_num = False
                        break
            align.append('r' if is_num else 'l')

    def fmt_cell(val, width, al):
        if al == 'r':
            return val.rjust(width)
        return val.ljust(width)

    sep = '  '
    lines = []

    # Header
    hdr = sep.join(fmt_cell(h, widths[i], align[i]) for i, h in enumerate(headers))
    lines.append(hdr)
    lines.append(sep.join('─' * w for w in widths))

    # Rows
    for row in str_rows:
        # Pad row if shorter than headers
        while len(row) < len(headers):
            row.append('')
        line = sep.join(fmt_cell(row[i], widths[i], align[i]) for i in range(len(headers)))
        lines.append(line)

    return '\n'.join(lines) + '\n'


def format_csv(headers, rows):
    """Format as CSV."""
    import csv
    import io
    buf = io.StringIO()
    writer = csv.writer(buf)
    writer.writerow(headers)
    for row in rows:
        writer.writerow(row)
    return buf.getvalue()


# ═════════════════════════════════════════════════════════════════════════════
#  File type detection
# ═════════════════════════════════════════════════════════════════════════════

def detect_type(data):
    """Auto-detect JSON file type from its structure."""
    if isinstance(data, dict):
        if data.get('format', '').startswith('prad2hvmon_settings'):
            return 'settings'
        if 'channels' in data and isinstance(data['channels'], list):
            return 'settings'

    if isinstance(data, list) and len(data) > 0:
        sample = data[0]
        if isinstance(sample, dict):
            keys = set(sample.keys())
            # hycal_modules: has "n", "t", "x", "y", "sx", "sy"
            if {'n', 't', 'x', 'y'}.issubset(keys):
                return 'modules'

    return None


# ═════════════════════════════════════════════════════════════════════════════
#  Extractors — turn JSON into (headers, rows)
# ═════════════════════════════════════════════════════════════════════════════

def extract_modules(data):
    """
    hycal_modules.json: [{n, t, x, y, sx, sy, ...}, ...]
    Separate boosters from detector modules.
    """
    det_headers = ['Name', 'Type', 'X', 'Y', 'SizeX', 'SizeY']
    det_rows = []
    bst_headers = ['Name', 'IP', 'Port']
    bst_rows = []

    # Collect any extra fields from detector modules
    extra_keys = OrderedDict()
    for m in data:
        t = m.get('t', '').lower()
        if t == 'booster':
            continue
        for k in m.keys():
            if k not in ('n', 't', 'x', 'y', 'sx', 'sy'):
                extra_keys[k] = True

    if extra_keys:
        det_headers += [k for k in extra_keys]

    for m in data:
        t = m.get('t', '').lower()
        if t == 'booster':
            bst_rows.append([
                m.get('n', ''),
                m.get('ip', ''),
                m.get('port', ''),
            ])
        else:
            row = [
                m.get('n', ''),
                m.get('t', ''),
                fmt_num(m.get('x')),
                fmt_num(m.get('y')),
                fmt_num(m.get('sx')),
                fmt_num(m.get('sy')),
            ]
            for k in extra_keys:
                row.append(fmt_num(m.get(k, '')))
            det_rows.append(row)

    return det_headers, det_rows, bst_headers, bst_rows


def extract_settings(data):
    """
    Settings JSON: {format, timestamp, channels: [{crate, slot, channel, name, params: {...}}, ...]}
    Output: address (crate/slot/ch), name, then each param as a column.
    """
    channels = data.get('channels', [])
    if not channels:
        return ['(no channels)'], []

    # Collect all param names across all channels
    param_names = OrderedDict()
    for ch in channels:
        params = ch.get('params', {})
        for k in params:
            param_names[k] = True

    headers = ['Crate', 'Slot', 'Ch', 'Name'] + list(param_names.keys())
    rows = []
    for ch in channels:
        params = ch.get('params', {})
        row = [
            ch.get('crate', ''),
            ch.get('slot', ''),
            ch.get('channel', ''),
            ch.get('name', ''),
        ]
        for pname in param_names:
            v = params.get(pname)
            row.append(fmt_num(v))
        rows.append(row)

    return headers, rows


def fmt_num(v):
    """Format a number nicely, pass through strings."""
    if v is None:
        return ''
    if isinstance(v, float):
        # Strip trailing zeros: 1500.00 → 1500, 1.50 → 1.5
        if v == int(v) and abs(v) < 1e9:
            return str(int(v))
        return f'{v:.2f}'.rstrip('0').rstrip('.')
    return str(v)


# ═════════════════════════════════════════════════════════════════════════════
#  Sorting / filtering
# ═════════════════════════════════════════════════════════════════════════════

def sort_rows(headers, rows, sort_col):
    """Sort rows by a column name (case-insensitive)."""
    hdr_lower = [h.lower() for h in headers]
    col_lower = sort_col.lower()
    if col_lower not in hdr_lower:
        print(f"Warning: sort column '{sort_col}' not found. "
              f"Available: {', '.join(headers)}", file=sys.stderr)
        return rows

    idx = hdr_lower.index(col_lower)

    def sort_key(row):
        val = row[idx] if idx < len(row) else ''
        # Try numeric sort
        try:
            return (0, float(val))
        except (ValueError, TypeError):
            return (1, str(val).lower())

    return sorted(rows, key=sort_key)


def filter_rows(headers, rows, expr):
    """
    Filter rows by expression: COL=VAL (exact) or COL~PATTERN (substring).
    """
    if '~' in expr:
        col_name, pattern = expr.split('~', 1)
        exact = False
    elif '=' in expr:
        col_name, pattern = expr.split('=', 1)
        exact = True
    else:
        print(f"Warning: invalid filter '{expr}'. Use COL=VAL or COL~PATTERN",
              file=sys.stderr)
        return rows

    hdr_lower = [h.lower() for h in headers]
    col_lower = col_name.strip().lower()
    pattern = pattern.strip()

    if col_lower not in hdr_lower:
        print(f"Warning: filter column '{col_name}' not found. "
              f"Available: {', '.join(headers)}", file=sys.stderr)
        return rows

    idx = hdr_lower.index(col_lower)
    result = []
    for row in rows:
        val = str(row[idx]) if idx < len(row) else ''
        if exact:
            if val.lower() == pattern.lower():
                result.append(row)
        else:
            if pattern.lower() in val.lower():
                result.append(row)

    return result


# ═════════════════════════════════════════════════════════════════════════════
#  Main
# ═════════════════════════════════════════════════════════════════════════════

def main():
    parser = argparse.ArgumentParser(
        description='Pretty-print PRad-II JSON config files as text tables.',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog='''\
examples:
  %(prog)s database/hycal_modules.json -s name
  %(prog)s snapshot.json -f "crate=PRadHV_1"
  %(prog)s snapshot.json -f "name~W2" -s name
  %(prog)s database/hycal_modules.json --csv > modules.csv
''')
    parser.add_argument('file', help='JSON file to display')
    parser.add_argument('-s', '--sort', metavar='COL',
                        help='Sort by column name')
    parser.add_argument('-f', '--filter', metavar='EXPR', action='append',
                        help='Filter: COL=VAL (exact) or COL~PAT (substring). Repeatable.')
    parser.add_argument('-t', '--type', choices=['modules', 'settings'],
                        help='Force file type (default: auto-detect)')
    parser.add_argument('-c', '--csv', action='store_true',
                        help='Output as CSV instead of table')
    parser.add_argument('-n', '--no-header', action='store_true',
                        help='Suppress the file-info header line')

    args = parser.parse_args()

    # Load JSON
    try:
        with open(args.file, 'r') as f:
            data = json.load(f)
    except FileNotFoundError:
        print(f"Error: file not found: {args.file}", file=sys.stderr)
        sys.exit(1)
    except json.JSONDecodeError as e:
        print(f"Error: invalid JSON: {e}", file=sys.stderr)
        sys.exit(1)

    # Detect type
    file_type = args.type or detect_type(data)
    if file_type is None:
        print("Error: cannot auto-detect file type. Use -t to specify.",
              file=sys.stderr)
        sys.exit(1)

    # Extract
    output_blocks = []   # list of (title, headers, rows)

    if file_type == 'modules':
        det_h, det_r, bst_h, bst_r = extract_modules(data)
        if det_r:
            output_blocks.append(('Detector Modules', det_h, det_r))
        if bst_r:
            output_blocks.append(('Booster Supplies', bst_h, bst_r))

    elif file_type == 'settings':
        headers, rows = extract_settings(data)
        ts = data.get('timestamp', '?')
        fmt_str = data.get('format', '?')
        title = f'Settings ({fmt_str}, {ts})'
        output_blocks.append((title, headers, rows))

    # Apply filter + sort to each block, then print
    for i, (title, headers, rows) in enumerate(output_blocks):
        # Filter
        if args.filter:
            for expr in args.filter:
                rows = filter_rows(headers, rows, expr)

        # Sort
        if args.sort:
            rows = sort_rows(headers, rows, args.sort)

        # Print
        if not args.no_header:
            fname = os.path.basename(args.file)
            print(f'═══ {title}  ({fname}, {len(rows)} rows) ═══')

        if args.csv:
            sys.stdout.write(format_csv(headers, rows))
        else:
            sys.stdout.write(format_table(headers, rows))

        if i < len(output_blocks) - 1:
            print()  # blank line between blocks


if __name__ == '__main__':
    main()
