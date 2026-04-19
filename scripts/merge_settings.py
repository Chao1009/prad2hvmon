#!/usr/bin/env python3
"""
merge_settings.py – Merge two prad2hvmon settings files.

Usage:
    python merge_settings.py -c <base.json> <new.json> [-o merged.json]
    python merge_settings.py -n <base.json> <new.json> [-o merged.json]

Modes:
    -c, --by-channel   Match by (crate, slot, channel).  Unique key.
                        Replaces name + all params from <new> into <base>.
    -n, --by-name      Match by channel name.
                        Skips any name that appears more than once in EITHER
                        file.  Replaces params only (not name).

The merged result starts from <base>; any channel in <new> that matches
a channel in <base> has its values overwritten.  Channels in <base> with
no match in <new> are kept unchanged.  Channels in <new> with no match
in <base> are ignored (not added).

Output goes to stdout unless -o is given.
"""

import argparse
import json
import sys
from collections import Counter
from datetime import datetime


def load_settings(path):
    with open(path) as f:
        data = json.load(f)
    fmt = data.get("format", "")
    if fmt != "prad2hvmon_settings_v1":
        print(f"Warning: unexpected format '{fmt}' in {path}", file=sys.stderr)
    return data


def channel_key(ch):
    """Unique hardware address tuple."""
    return (ch["crate"], ch["slot"], ch["channel"])


def find_duplicate_names(channels):
    """Return the set of names that appear more than once."""
    counts = Counter(ch["name"] for ch in channels)
    return {name for name, n in counts.items() if n > 1}


def merge_by_channel(base, new):
    """Merge by (crate, slot, channel).  Replaces name + params."""
    new_map = {channel_key(ch): ch for ch in new["channels"]}

    merged_channels = []
    replaced = 0
    for ch in base["channels"]:
        key = channel_key(ch)
        if key in new_map:
            src = new_map[key]
            out = dict(ch)                        # start from base entry
            out["name"] = src["name"]             # overwrite name
            out["params"] = dict(ch.get("params", {}))
            out["params"].update(src.get("params", {}))  # overwrite params
            merged_channels.append(out)
            replaced += 1
        else:
            merged_channels.append(ch)

    base_keys = {channel_key(ch) for ch in base["channels"]}
    new_only = [channel_key(ch) for ch in new["channels"] if channel_key(ch) not in base_keys]

    print(f"By-channel merge: {replaced} replaced, "
          f"{len(base['channels']) - replaced} unchanged, "
          f"{len(new_only)} in <new> with no match in <base> (ignored)",
          file=sys.stderr)

    return merged_channels, []  # no skipped names in channel mode


def merge_by_name(base, new):
    """Merge by name.  Replaces params only.  Skips duplicate names."""
    dup_base = find_duplicate_names(base["channels"])
    dup_new  = find_duplicate_names(new["channels"])

    # Build lookup from non-duplicate names in <new>
    new_map = {}
    for ch in new["channels"]:
        name = ch["name"]
        if name not in dup_new:
            new_map[name] = ch

    skipped_names = sorted(dup_base | dup_new)

    merged_channels = []
    replaced = 0
    skipped_in_merge = 0
    for ch in base["channels"]:
        name = ch["name"]
        if name in dup_base:
            # base name is ambiguous — keep as-is
            merged_channels.append(ch)
            skipped_in_merge += 1
            continue
        if name in new_map:
            src = new_map[name]
            out = dict(ch)
            out["params"] = dict(ch.get("params", {}))
            out["params"].update(src.get("params", {}))  # overwrite params only
            merged_channels.append(out)
            replaced += 1
        else:
            merged_channels.append(ch)

    # Count unique names in <new> that have no match in base
    base_unique_names = {ch["name"] for ch in base["channels"]
                         if ch["name"] not in dup_base}
    new_only = [name for name in new_map if name not in base_unique_names]

    print(f"By-name merge: {replaced} replaced, "
          f"{len(base['channels']) - replaced - skipped_in_merge} unchanged, "
          f"{skipped_in_merge} skipped (ambiguous name in base), "
          f"{len(new_only)} in <new> with no match in <base> (ignored)",
          file=sys.stderr)

    return merged_channels, skipped_names


def main():
    parser = argparse.ArgumentParser(
        description="Merge two prad2hvmon settings files.")
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("-c", "--by-channel", action="store_true",
                      help="Match by (crate, slot, channel)")
    mode.add_argument("-n", "--by-name", action="store_true",
                      help="Match by channel name (skips duplicates)")
    parser.add_argument("base", help="Base settings file")
    parser.add_argument("new", help="New settings file (values override base)")
    parser.add_argument("-o", "--output", default=None,
                        help="Output file (default: stdout)")
    args = parser.parse_args()

    base = load_settings(args.base)
    new  = load_settings(args.new)

    if args.by_channel:
        merged_channels, skipped = merge_by_channel(base, new)
    else:
        merged_channels, skipped = merge_by_name(base, new)

    # Build output
    result = {
        "format":    "prad2hvmon_settings_v1",
        "timestamp": datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
        "merged_from": {
            "base": args.base,
            "new":  args.new,
            "mode": "by-channel" if args.by_channel else "by-name",
        },
        "channels": merged_channels,
    }

    out_str = json.dumps(result, indent=2)

    if args.output:
        with open(args.output, "w") as f:
            f.write(out_str + "\n")
        print(f"Written to {args.output} ({len(merged_channels)} channels)",
              file=sys.stderr)
    else:
        print(out_str)

    # Report skipped names (by-name mode only)
    if skipped:
        print(f"\nSkipped names (duplicate in base and/or new):", file=sys.stderr)
        for name in skipped:
            sources = []
            if name in find_duplicate_names(base["channels"]):
                sources.append("base")
            if name in find_duplicate_names(new["channels"]):
                sources.append("new")
            print(f"  {name}  (duplicate in {', '.join(sources)})", file=sys.stderr)


if __name__ == "__main__":
    main()
