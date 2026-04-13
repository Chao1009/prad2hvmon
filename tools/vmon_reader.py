#!/usr/bin/env python3
"""
vmon_reader.py — Reader for VMDF v1 binary files produced by prad2hvd.

Usage examples:

  # Print file summary
  python vmon_reader.py summary vmon_20260413.dat

  # List channel names (from the latest channel table)
  python vmon_reader.py channels vmon_20260413.dat

  # Export one channel to CSV
  python vmon_reader.py csv vmon_20260413.dat --channel G1

  # Export all channels to a single CSV (wide format)
  python vmon_reader.py csv vmon_20260413.dat --all

  # Plot one or more channels
  python vmon_reader.py plot vmon_20260413.dat --channel G1 G2 W100

  # Compute per-channel statistics (mean, std, min, max)
  python vmon_reader.py stats vmon_20260413.dat

  # Read multiple files (e.g. a whole week)
  python vmon_reader.py plot vmon_202604*.dat --channel G1
"""

import argparse
import struct
import sys
import os
from datetime import datetime, timezone
from pathlib import Path
import numpy as np

# ═══════════════════════════════════════════════════════════════════════
#  VMDF v1 binary reader
# ═══════════════════════════════════════════════════════════════════════

MAGIC = b"VMD1"
TAG_CHTABLE = 0x01
TAG_VMON    = 0x02
NAME_LEN    = 12


class VMDFReader:
    """
    Reads one .dat file produced by VMonRecorder.

    Attributes after read():
        n_channels   int
        interval_ms  int
        t0_epoch_ms  int
        names        list[str]   — channel names (from last table record)
        timestamps   np.ndarray  — epoch-ms for each snapshot (float64)
        data         np.ndarray  — shape (n_snapshots, n_channels), float32
                                   NaN for missing/offline channels
    """

    def __init__(self):
        self.n_channels  = 0
        self.interval_ms = 0
        self.t0_epoch_ms = 0
        self.names       = []
        self.timestamps  = np.empty(0, dtype=np.float64)
        self.data        = np.empty((0, 0), dtype=np.float32)

    def read(self, path):
        """Read a single VMDF file. Returns self for chaining."""
        path = str(path)
        with open(path, "rb") as f:
            buf = f.read()

        if len(buf) < 20:
            raise ValueError(f"{path}: file too small ({len(buf)} bytes)")

        # ── Header ───────────────────────────────────────────────────
        magic = buf[0:4]
        if magic != MAGIC:
            raise ValueError(f"{path}: bad magic {magic!r}")

        ver, n_ch, interval, flags, t0 = struct.unpack_from("<HHHHq", buf, 4)
        if ver != 1:
            raise ValueError(f"{path}: unsupported version {ver}")

        self.n_channels  = n_ch
        self.interval_ms = interval
        self.t0_epoch_ms = t0

        chtable_size = 1 + 4 + n_ch * NAME_LEN
        vmon_size    = 1 + 4 + n_ch * 4

        # ── Scan records ─────────────────────────────────────────────
        pos = 20
        end = len(buf)
        names = [f"ch{i}" for i in range(n_ch)]  # fallback
        ts_list  = []
        val_list = []

        while pos < end:
            if pos + 1 > end:
                break
            tag = buf[pos]

            if tag == TAG_CHTABLE:
                if pos + chtable_size > end:
                    break  # partial
                dt = struct.unpack_from("<I", buf, pos + 1)[0]
                name_block = buf[pos + 5 : pos + 5 + n_ch * NAME_LEN]
                names = []
                for i in range(n_ch):
                    raw = name_block[i * NAME_LEN : (i + 1) * NAME_LEN]
                    names.append(raw.split(b"\x00", 1)[0].decode("ascii", errors="replace"))
                pos += chtable_size

            elif tag == TAG_VMON:
                if pos + vmon_size > end:
                    break  # partial
                dt = struct.unpack_from("<I", buf, pos + 1)[0]
                vals = np.frombuffer(buf, dtype=np.float32, count=n_ch, offset=pos + 5)
                ts_list.append(t0 + dt)
                val_list.append(vals.copy())
                pos += vmon_size

            else:
                # Unknown tag — treat as corruption, stop
                break

        self.names = names
        if val_list:
            self.timestamps = np.array(ts_list, dtype=np.float64)
            self.data = np.stack(val_list)   # (n_snapshots, n_channels)
        else:
            self.timestamps = np.empty(0, dtype=np.float64)
            self.data = np.empty((0, n_ch), dtype=np.float32)

        return self

    @property
    def n_snapshots(self):
        return len(self.timestamps)

    @property
    def duration_s(self):
        if self.n_snapshots < 2:
            return 0.0
        return (self.timestamps[-1] - self.timestamps[0]) / 1000.0

    def channel_index(self, name):
        """Return the column index for a channel name, or None."""
        try:
            return self.names.index(name)
        except ValueError:
            return None

    def channel_data(self, name):
        """Return (timestamps_ms, vmon_array) for one channel."""
        idx = self.channel_index(name)
        if idx is None:
            raise KeyError(f"Channel '{name}' not found")
        return self.timestamps.copy(), self.data[:, idx].copy()

    def t0_datetime(self):
        return datetime.fromtimestamp(self.t0_epoch_ms / 1000.0, tz=timezone.utc)


def read_files(paths):
    """Read and concatenate multiple VMDF files. Returns a merged VMDFReader."""
    readers = []
    for p in sorted(paths):
        r = VMDFReader()
        try:
            r.read(p)
            readers.append(r)
        except Exception as e:
            print(f"Warning: skipping {p}: {e}", file=sys.stderr)

    if not readers:
        print("No valid files.", file=sys.stderr)
        sys.exit(1)

    if len(readers) == 1:
        return readers[0]

    # Merge: concatenate timestamps and data
    merged = VMDFReader()
    merged.n_channels  = readers[0].n_channels
    merged.interval_ms = readers[0].interval_ms
    merged.t0_epoch_ms = readers[0].t0_epoch_ms
    merged.names       = readers[-1].names   # use latest name table
    merged.timestamps  = np.concatenate([r.timestamps for r in readers])
    merged.data        = np.concatenate([r.data for r in readers], axis=0)
    return merged


# ═══════════════════════════════════════════════════════════════════════
#  CLI commands
# ═══════════════════════════════════════════════════════════════════════

def cmd_summary(args):
    for path in args.files:
        r = VMDFReader().read(path)
        t0 = r.t0_datetime()
        print(f"File:       {path}")
        print(f"Channels:   {r.n_channels}")
        print(f"Interval:   {r.interval_ms} ms")
        print(f"Start:      {t0.strftime('%Y-%m-%d %H:%M:%S')} UTC")
        print(f"Snapshots:  {r.n_snapshots:,}")
        print(f"Duration:   {r.duration_s:.1f} s ({r.duration_s/3600:.2f} h)")
        size = os.path.getsize(path)
        print(f"File size:  {size:,} bytes ({size/1e6:.1f} MB)")
        if r.n_snapshots > 0:
            # Count non-NaN channels in first snapshot
            valid = np.count_nonzero(~np.isnan(r.data[0]))
            print(f"Active ch:  {valid} / {r.n_channels}")
        print()


def cmd_channels(args):
    r = read_files(args.files)
    for i, name in enumerate(r.names):
        print(f"{i:4d}  {name}")


def cmd_stats(args):
    r = read_files(args.files)
    if r.n_snapshots == 0:
        print("No data.")
        return

    print(f"{'Channel':>12s}  {'Mean':>10s}  {'Std':>10s}  {'Min':>10s}  {'Max':>10s}  {'N':>8s}")
    print("-" * 70)
    for i, name in enumerate(r.names):
        col = r.data[:, i]
        valid = col[~np.isnan(col)]
        if len(valid) == 0:
            print(f"{name:>12s}  {'—':>10s}  {'—':>10s}  {'—':>10s}  {'—':>10s}  {0:>8d}")
        else:
            print(f"{name:>12s}  {np.mean(valid):10.4f}  {np.std(valid):10.4f}  "
                  f"{np.min(valid):10.4f}  {np.max(valid):10.4f}  {len(valid):>8d}")


def cmd_csv(args):
    r = read_files(args.files)
    if r.n_snapshots == 0:
        print("No data.", file=sys.stderr)
        return

    out = open(args.output, "w") if args.output else sys.stdout

    if args.all:
        # Wide format: timestamp, ch0, ch1, ...
        out.write("epoch_ms," + ",".join(r.names) + "\n")
        for i in range(r.n_snapshots):
            vals = ",".join(f"{r.data[i,j]:.2f}" if not np.isnan(r.data[i, j]) else ""
                           for j in range(r.n_channels))
            out.write(f"{r.timestamps[i]:.0f},{vals}\n")
    else:
        # Single-channel or filtered
        channels = args.channel or []
        if not channels:
            print("Specify --channel NAME or --all", file=sys.stderr)
            return
        out.write("epoch_ms,rel_ms,channel,vmon\n")
        for ch_name in channels:
            idx = r.channel_index(ch_name)
            if idx is None:
                print(f"Warning: channel '{ch_name}' not found, skipping",
                      file=sys.stderr)
                continue
            t0 = r.timestamps[0]
            for i in range(r.n_snapshots):
                v = r.data[i, idx]
                if not np.isnan(v):
                    out.write(f"{r.timestamps[i]:.0f},{r.timestamps[i]-t0:.0f},"
                              f"{ch_name},{v:.2f}\n")

    if args.output:
        out.close()
        print(f"Written to {args.output}", file=sys.stderr)


def cmd_plot(args):
    try:
        import matplotlib.pyplot as plt
    except ImportError:
        print("matplotlib is required for plotting: pip install matplotlib",
              file=sys.stderr)
        sys.exit(1)

    r = read_files(args.files)
    if r.n_snapshots == 0:
        print("No data.", file=sys.stderr)
        return

    channels = args.channel or []
    if not channels:
        print("Specify --channel NAME [NAME ...]", file=sys.stderr)
        return

    fig, ax = plt.subplots(figsize=(12, 5))
    t0 = r.timestamps[0]
    rel_s = (r.timestamps - t0) / 1000.0  # seconds

    for ch_name in channels:
        idx = r.channel_index(ch_name)
        if idx is None:
            print(f"Warning: channel '{ch_name}' not found", file=sys.stderr)
            continue
        col = r.data[:, idx]
        mask = ~np.isnan(col)
        ax.plot(rel_s[mask], col[mask], label=ch_name, linewidth=0.5)

    ax.set_xlabel("Time [s]")
    ax.set_ylabel("VMon [V]")
    ax.legend()
    ax.grid(True, alpha=0.3)
    start_str = r.t0_datetime().strftime("%Y-%m-%d %H:%M:%S UTC")
    ax.set_title(f"VMon — start {start_str}")
    plt.tight_layout()

    if args.output:
        fig.savefig(args.output, dpi=150)
        print(f"Saved to {args.output}", file=sys.stderr)
    else:
        plt.show()


# ═══════════════════════════════════════════════════════════════════════
#  Main
# ═══════════════════════════════════════════════════════════════════════

def main():
    ap = argparse.ArgumentParser(
        description="Read VMDF v1 binary files produced by prad2hvd",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    sub = ap.add_subparsers(dest="command", required=True)

    # summary
    p = sub.add_parser("summary", help="Print file summary")
    p.add_argument("files", nargs="+", help="VMDF .dat files")

    # channels
    p = sub.add_parser("channels", help="List channel names")
    p.add_argument("files", nargs="+")

    # stats
    p = sub.add_parser("stats", help="Per-channel statistics")
    p.add_argument("files", nargs="+")

    # csv
    p = sub.add_parser("csv", help="Export to CSV")
    p.add_argument("files", nargs="+")
    p.add_argument("--channel", "-c", nargs="*", help="Channel name(s)")
    p.add_argument("--all", "-a", action="store_true", help="Export all channels (wide format)")
    p.add_argument("--output", "-o", help="Output file (default: stdout)")

    # plot
    p = sub.add_parser("plot", help="Plot channel traces")
    p.add_argument("files", nargs="+")
    p.add_argument("--channel", "-c", nargs="+", required=True, help="Channel name(s)")
    p.add_argument("--output", "-o", help="Save plot to file instead of showing")

    args = ap.parse_args()

    dispatch = {
        "summary":  cmd_summary,
        "channels": cmd_channels,
        "stats":    cmd_stats,
        "csv":      cmd_csv,
        "plot":     cmd_plot,
    }
    dispatch[args.command](args)


if __name__ == "__main__":
    main()
