#!/usr/bin/env python3
"""
vmon_reader.py — Reader for VMDF v2 binary files produced by prad2hvd.

For HV channels the daemon records dV = VMon - V0Set at every fast-poll
cycle.  Channel-table records (emit-on-change) carry the name + V0Set
active at that moment, so the absolute VMon can be reconstructed as
dV + V0Set(active at snapshot time).

Booster (TDK-Lambda) supplies are logged alongside when present.  Their
setpoints (VSet/ISet) live in a similar emit-on-change table, and their
VMon/IMon measurements are written only when they actually change — so
the file naturally carries the booster-poll rate rather than duplicating
one sample per HV fast poll.

Usage examples:

  python vmon_reader.py summary vmon_20260413.dat
  python vmon_reader.py channels vmon_20260413.dat
  python vmon_reader.py stats vmon_20260413.dat
  python vmon_reader.py csv vmon_20260413.dat --channel G1
  python vmon_reader.py csv vmon_20260413.dat --channel G1 --absolute
  python vmon_reader.py plot vmon_20260413.dat --channel G1 G2 W100
  python vmon_reader.py plot vmon_20260413.dat --channel 'Booster 1'
  python vmon_reader.py plot vmon_20260413.dat --channel 'Booster 1' --imon
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
#  VMDF v2 binary reader
# ═══════════════════════════════════════════════════════════════════════

MAGIC = b"VMD2"
TAG_CHTABLE       = 0x01
TAG_DV            = 0x02
TAG_BOOSTER_TABLE = 0x03
TAG_BOOSTER       = 0x04

NAME_LEN        = 12
CHREC_BYTES     = NAME_LEN + 4        # name[12] + V0Set(float32)
BST_TABLE_BYTES = NAME_LEN + 4 + 4    # name + VSet + ISet
BST_SNAP_BYTES  = 4 + 4               # VMon + IMon


class VMDFReader:
    """
    Reads one .dat file produced by VMonRecorder.

    HV-channel attributes (after read()):
        n_channels   int
        interval_ms  int
        t0_epoch_ms  int
        names        list[str]          — latest channel names
        v0sets       np.ndarray(n_ch,)  — latest V0Set values
        timestamps   np.ndarray         — epoch-ms per dV snapshot (float64)
        data         np.ndarray(n_snap, n_ch) — dV, float32 (NaN if offline)
        ch_events    list[dict]         — channel-table history; each:
                                              abs_ts : float (epoch-ms)
                                              names  : list[str]
                                              v0sets : np.ndarray(n_ch,) float32

    Booster attributes (empty when n_boosters == 0):
        n_boosters          int
        booster_names       list[str]
        booster_vsets       np.ndarray(n_bst,) float32   — latest
        booster_isets       np.ndarray(n_bst,) float32   — latest
        booster_timestamps  np.ndarray        — epoch-ms per booster snapshot
        booster_vmon        np.ndarray(n_bst_snap, n_bst) float32
        booster_imon        np.ndarray(n_bst_snap, n_bst) float32
        booster_events      list[dict]        — booster-table history; each:
                                                    abs_ts : float
                                                    names  : list[str]
                                                    vsets  : np.ndarray float32
                                                    isets  : np.ndarray float32
    """

    def __init__(self):
        self.n_channels  = 0
        self.interval_ms = 0
        self.t0_epoch_ms = 0
        self.names       = []
        self.v0sets      = np.empty(0, dtype=np.float32)
        self.timestamps  = np.empty(0, dtype=np.float64)
        self.data        = np.empty((0, 0), dtype=np.float32)
        self.ch_events   = []

        self.n_boosters         = 0
        self.booster_names      = []
        self.booster_vsets      = np.empty(0, dtype=np.float32)
        self.booster_isets      = np.empty(0, dtype=np.float32)
        self.booster_timestamps = np.empty(0, dtype=np.float64)
        self.booster_vmon       = np.empty((0, 0), dtype=np.float32)
        self.booster_imon       = np.empty((0, 0), dtype=np.float32)
        self.booster_events     = []

    # ── File parser ──────────────────────────────────────────────────────

    def read(self, path):
        path = str(path)
        with open(path, "rb") as f:
            buf = f.read()

        if len(buf) < 20:
            raise ValueError(f"{path}: file too small ({len(buf)} bytes)")

        magic = buf[0:4]
        if magic != MAGIC:
            if magic == b"VMD1":
                raise ValueError(
                    f"{path}: legacy VMD1 file — use an older vmon_reader.py "
                    f"(this reader speaks VMD2, which records dV + boosters)"
                )
            raise ValueError(f"{path}: bad magic {magic!r}")

        ver, n_ch, interval, n_bst, t0 = struct.unpack_from("<HHHHq", buf, 4)
        if ver != 2:
            raise ValueError(f"{path}: unsupported version {ver}")

        self.n_channels  = n_ch
        self.interval_ms = interval
        self.t0_epoch_ms = t0
        self.n_boosters  = n_bst

        chtable_size   = 1 + 4 + n_ch  * CHREC_BYTES
        dv_size        = 1 + 4 + n_ch  * 4
        bst_table_size = 1 + 4 + n_bst * BST_TABLE_BYTES
        bst_snap_size  = 1 + 4 + n_bst * BST_SNAP_BYTES

        pos = 20
        end = len(buf)
        names  = [f"ch{i}" for i in range(n_ch)]
        v0sets = np.full(n_ch, np.nan, dtype=np.float32)
        ts_list, val_list = [], []
        ch_events = []

        b_names = [f"bst{i}" for i in range(n_bst)]
        b_vsets = np.full(n_bst, np.nan, dtype=np.float32)
        b_isets = np.full(n_bst, np.nan, dtype=np.float32)
        b_ts_list, b_vm_list, b_im_list = [], [], []
        booster_events = []

        while pos < end:
            if pos + 1 > end:
                break
            tag = buf[pos]

            if tag == TAG_CHTABLE:
                if pos + chtable_size > end:
                    break
                dt = struct.unpack_from("<I", buf, pos + 1)[0]
                tbl = buf[pos + 5 : pos + 5 + n_ch * CHREC_BYTES]
                names = []
                v0_arr = np.empty(n_ch, dtype=np.float32)
                for i in range(n_ch):
                    base = i * CHREC_BYTES
                    names.append(
                        tbl[base : base + NAME_LEN].split(b"\x00", 1)[0]
                            .decode("ascii", errors="replace")
                    )
                    v0_arr[i] = struct.unpack_from("<f", tbl, base + NAME_LEN)[0]
                v0sets = v0_arr
                ch_events.append({
                    "abs_ts": float(t0 + dt),
                    "names":  names,
                    "v0sets": v0_arr,
                })
                pos += chtable_size

            elif tag == TAG_DV:
                if pos + dv_size > end:
                    break
                dt = struct.unpack_from("<I", buf, pos + 1)[0]
                vals = np.frombuffer(buf, dtype=np.float32, count=n_ch,
                                     offset=pos + 5)
                ts_list.append(t0 + dt)
                val_list.append(vals.copy())
                pos += dv_size

            elif tag == TAG_BOOSTER_TABLE:
                if pos + bst_table_size > end:
                    break
                dt = struct.unpack_from("<I", buf, pos + 1)[0]
                tbl = buf[pos + 5 : pos + 5 + n_bst * BST_TABLE_BYTES]
                bn = []
                vs_arr = np.empty(n_bst, dtype=np.float32)
                is_arr = np.empty(n_bst, dtype=np.float32)
                for i in range(n_bst):
                    base = i * BST_TABLE_BYTES
                    bn.append(
                        tbl[base : base + NAME_LEN].split(b"\x00", 1)[0]
                            .decode("ascii", errors="replace")
                    )
                    vs_arr[i] = struct.unpack_from("<f", tbl, base + NAME_LEN)[0]
                    is_arr[i] = struct.unpack_from("<f", tbl, base + NAME_LEN + 4)[0]
                b_names, b_vsets, b_isets = bn, vs_arr, is_arr
                booster_events.append({
                    "abs_ts": float(t0 + dt),
                    "names":  bn,
                    "vsets":  vs_arr,
                    "isets":  is_arr,
                })
                pos += bst_table_size

            elif tag == TAG_BOOSTER:
                if pos + bst_snap_size > end:
                    break
                dt = struct.unpack_from("<I", buf, pos + 1)[0]
                vm = np.empty(n_bst, dtype=np.float32)
                im = np.empty(n_bst, dtype=np.float32)
                for i in range(n_bst):
                    base = pos + 5 + i * BST_SNAP_BYTES
                    vm[i] = struct.unpack_from("<f", buf, base)[0]
                    im[i] = struct.unpack_from("<f", buf, base + 4)[0]
                b_ts_list.append(t0 + dt)
                b_vm_list.append(vm)
                b_im_list.append(im)
                pos += bst_snap_size

            else:
                break  # unknown tag = corruption

        self.names         = names
        self.v0sets        = v0sets
        self.ch_events     = ch_events
        if val_list:
            self.timestamps = np.array(ts_list, dtype=np.float64)
            self.data       = np.stack(val_list)
        else:
            self.timestamps = np.empty(0, dtype=np.float64)
            self.data       = np.empty((0, n_ch), dtype=np.float32)

        self.booster_names   = b_names
        self.booster_vsets   = b_vsets
        self.booster_isets   = b_isets
        self.booster_events  = booster_events
        if b_vm_list:
            self.booster_timestamps = np.array(b_ts_list, dtype=np.float64)
            self.booster_vmon       = np.stack(b_vm_list)
            self.booster_imon       = np.stack(b_im_list)
        else:
            self.booster_timestamps = np.empty(0, dtype=np.float64)
            self.booster_vmon       = np.empty((0, n_bst), dtype=np.float32)
            self.booster_imon       = np.empty((0, n_bst), dtype=np.float32)

        return self

    # ── Metadata helpers ─────────────────────────────────────────────────

    @property
    def n_snapshots(self):
        return len(self.timestamps)

    @property
    def n_booster_snapshots(self):
        return len(self.booster_timestamps)

    @property
    def duration_s(self):
        if self.n_snapshots < 2:
            return 0.0
        return (self.timestamps[-1] - self.timestamps[0]) / 1000.0

    def t0_datetime(self):
        return datetime.fromtimestamp(self.t0_epoch_ms / 1000.0, tz=timezone.utc)

    # ── Lookup ───────────────────────────────────────────────────────────

    def channel_index(self, name):
        try:
            return self.names.index(name)
        except ValueError:
            return None

    def booster_index(self, name):
        try:
            return self.booster_names.index(name)
        except ValueError:
            return None

    def resolve(self, name):
        """Return ('hv', idx) | ('booster', idx) | None."""
        idx = self.channel_index(name)
        if idx is not None:
            return ("hv", idx)
        idx = self.booster_index(name)
        if idx is not None:
            return ("booster", idx)
        return None

    # ── HV V0Set piecewise reconstruction ────────────────────────────────

    def v0set_trace(self, ch_idx):
        n = self.n_snapshots
        if n == 0:
            return np.empty(0, dtype=np.float32)
        if not self.ch_events:
            return np.full(n, np.nan, dtype=np.float32)

        event_ts = np.array([ev["abs_ts"] for ev in self.ch_events])
        idx = np.searchsorted(event_ts, self.timestamps, side="right") - 1
        idx = np.clip(idx, 0, len(self.ch_events) - 1)
        per_event = np.array([ev["v0sets"][ch_idx] for ev in self.ch_events],
                             dtype=np.float32)
        return per_event[idx]

    def channel_data(self, name, absolute=False):
        idx = self.channel_index(name)
        if idx is None:
            raise KeyError(f"HV channel '{name}' not found")
        dv = self.data[:, idx].copy()
        if absolute:
            dv = dv + self.v0set_trace(idx)
        return self.timestamps.copy(), dv

    # ── Booster trace access ─────────────────────────────────────────────

    def booster_trace(self, name, kind="vmon"):
        """Return (timestamps_ms, values) for one booster.

        kind: 'vmon' | 'imon' | 'vset' | 'iset'
              vmon/imon use the sparse booster snapshot stream.
              vset/iset use the piecewise-constant table events evaluated
              at the booster snapshot timestamps.
        """
        idx = self.booster_index(name)
        if idx is None:
            raise KeyError(f"Booster '{name}' not found")

        if kind in ("vmon", "imon"):
            src = self.booster_vmon if kind == "vmon" else self.booster_imon
            return self.booster_timestamps.copy(), src[:, idx].copy()

        # vset / iset: piecewise-constant reconstruction at snapshot times
        n = self.n_booster_snapshots
        if n == 0 or not self.booster_events:
            return self.booster_timestamps.copy(), np.empty(0, dtype=np.float32)
        event_ts = np.array([ev["abs_ts"] for ev in self.booster_events])
        ei = np.searchsorted(event_ts, self.booster_timestamps, side="right") - 1
        ei = np.clip(ei, 0, len(self.booster_events) - 1)
        key = "vsets" if kind == "vset" else "isets"
        per_event = np.array([ev[key][idx] for ev in self.booster_events],
                             dtype=np.float32)
        return self.booster_timestamps.copy(), per_event[ei]


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

    merged = VMDFReader()
    merged.n_channels  = readers[0].n_channels
    merged.interval_ms = readers[0].interval_ms
    merged.t0_epoch_ms = readers[0].t0_epoch_ms
    merged.names       = readers[-1].names
    merged.v0sets      = readers[-1].v0sets
    merged.timestamps  = np.concatenate([r.timestamps for r in readers])
    merged.data        = np.concatenate([r.data for r in readers], axis=0)
    merged.ch_events   = [ev for r in readers for ev in r.ch_events]

    # Only merge booster data from files whose booster count matches the
    # first file.  If it diverges mid-range we drop the outliers rather than
    # produce shape-mismatched arrays.
    ref_n_bst = readers[0].n_boosters
    compat = [r for r in readers if r.n_boosters == ref_n_bst]
    dropped = len(readers) - len(compat)
    if dropped:
        print(f"Warning: {dropped} file(s) skipped for booster merge "
              f"(n_boosters != {ref_n_bst})", file=sys.stderr)

    merged.n_boosters = ref_n_bst
    if compat:
        merged.booster_names      = compat[-1].booster_names
        merged.booster_vsets      = compat[-1].booster_vsets
        merged.booster_isets      = compat[-1].booster_isets
        merged.booster_timestamps = np.concatenate(
            [r.booster_timestamps for r in compat])
        merged.booster_vmon = np.concatenate(
            [r.booster_vmon for r in compat], axis=0) if ref_n_bst else \
            np.empty((0, 0), dtype=np.float32)
        merged.booster_imon = np.concatenate(
            [r.booster_imon for r in compat], axis=0) if ref_n_bst else \
            np.empty((0, 0), dtype=np.float32)
        merged.booster_events = [ev for r in compat for ev in r.booster_events]
    else:
        merged.booster_names      = []
        merged.booster_vsets      = np.empty(0, dtype=np.float32)
        merged.booster_isets      = np.empty(0, dtype=np.float32)
        merged.booster_timestamps = np.empty(0, dtype=np.float64)
        merged.booster_vmon       = np.empty((0, ref_n_bst), dtype=np.float32)
        merged.booster_imon       = np.empty((0, ref_n_bst), dtype=np.float32)
        merged.booster_events     = []
    return merged


# ═══════════════════════════════════════════════════════════════════════
#  CLI commands
# ═══════════════════════════════════════════════════════════════════════

def cmd_summary(args):
    for path in args.files:
        r = VMDFReader().read(path)
        t0 = r.t0_datetime()
        print(f"File:         {path}")
        print(f"HV channels:  {r.n_channels}")
        print(f"Boosters:     {r.n_boosters}")
        print(f"Interval:     {r.interval_ms} ms")
        print(f"Start:        {t0.strftime('%Y-%m-%d %H:%M:%S')} UTC")
        print(f"dV snaps:     {r.n_snapshots:,}")
        print(f"Bst snaps:    {r.n_booster_snapshots:,}")
        print(f"Duration:     {r.duration_s:.1f} s ({r.duration_s/3600:.2f} h)")
        print(f"Ch tables:    {len(r.ch_events)}")
        print(f"Bst tables:   {len(r.booster_events)}")
        size = os.path.getsize(path)
        print(f"File size:    {size:,} bytes ({size/1e6:.1f} MB)")
        if r.n_snapshots > 0:
            valid = np.count_nonzero(~np.isnan(r.data[0]))
            print(f"Active ch:    {valid} / {r.n_channels}")
        print()


def cmd_channels(args):
    r = read_files(args.files)
    print("HV channels:")
    print(f"  {'idx':>4s}  {'name':<12s}  {'V0Set':>10s}")
    for i, name in enumerate(r.names):
        v = r.v0sets[i] if i < len(r.v0sets) else float("nan")
        v_str = f"{v:10.2f}" if not np.isnan(v) else f"{'—':>10s}"
        print(f"  {i:4d}  {name:<12s}  {v_str}")
    if r.n_boosters:
        print("\nBoosters:")
        print(f"  {'idx':>4s}  {'name':<12s}  {'VSet':>10s}  {'ISet':>10s}")
        for i, name in enumerate(r.booster_names):
            vs = r.booster_vsets[i]
            is_ = r.booster_isets[i]
            vs_str = f"{vs:10.2f}" if not np.isnan(vs) else f"{'—':>10s}"
            is_str = f"{is_:10.3f}" if not np.isnan(is_) else f"{'—':>10s}"
            print(f"  {i:4d}  {name:<12s}  {vs_str}  {is_str}")


def _stats_row(name, arr):
    valid = arr[~np.isnan(arr)]
    if len(valid) == 0:
        return (f"{name:>14s}  {'—':>10s}  {'—':>10s}  {'—':>10s}  "
                f"{'—':>10s}  {0:>8d}")
    return (f"{name:>14s}  {np.mean(valid):10.4f}  {np.std(valid):10.4f}  "
            f"{np.min(valid):10.4f}  {np.max(valid):10.4f}  {len(valid):>8d}")


def cmd_stats(args):
    r = read_files(args.files)
    if r.n_snapshots == 0 and r.n_booster_snapshots == 0:
        print("No data.")
        return

    label = "VMon" if args.absolute else "dV"
    print(f"HV statistics on {label}")
    print(f"{'Channel':>14s}  {'Mean':>10s}  {'Std':>10s}  "
          f"{'Min':>10s}  {'Max':>10s}  {'N':>8s}")
    print("-" * 72)
    for i, name in enumerate(r.names):
        col = r.data[:, i]
        if args.absolute:
            col = col + r.v0set_trace(i)
        print(_stats_row(name, col))

    if r.n_boosters:
        print(f"\nBooster statistics on VMon")
        print(f"{'Booster':>14s}  {'Mean':>10s}  {'Std':>10s}  "
              f"{'Min':>10s}  {'Max':>10s}  {'N':>8s}")
        print("-" * 72)
        for i, name in enumerate(r.booster_names):
            print(_stats_row(name, r.booster_vmon[:, i]))
        print(f"\nBooster statistics on IMon")
        print(f"{'Booster':>14s}  {'Mean':>10s}  {'Std':>10s}  "
              f"{'Min':>10s}  {'Max':>10s}  {'N':>8s}")
        print("-" * 72)
        for i, name in enumerate(r.booster_names):
            print(_stats_row(name, r.booster_imon[:, i]))


def cmd_csv(args):
    r = read_files(args.files)

    out = open(args.output, "w") if args.output else sys.stdout

    if args.all:
        # Wide format: HV channels (dV or absolute), no boosters
        if r.n_snapshots == 0:
            print("No HV data.", file=sys.stderr)
            return
        out.write("epoch_ms," + ",".join(r.names) + "\n")
        if args.absolute:
            event_ts = (np.array([ev["abs_ts"] for ev in r.ch_events])
                        if r.ch_events else np.empty(0))
            if len(event_ts):
                idx = np.searchsorted(event_ts, r.timestamps, side="right") - 1
                idx = np.clip(idx, 0, len(r.ch_events) - 1)
                v0_matrix = np.stack([ev["v0sets"] for ev in r.ch_events])
            else:
                idx = np.zeros(r.n_snapshots, dtype=int)
                v0_matrix = np.full((1, r.n_channels), np.nan, dtype=np.float32)
            for i in range(r.n_snapshots):
                row = r.data[i] + v0_matrix[idx[i]]
                vals = ",".join(f"{row[j]:.2f}" if not np.isnan(row[j]) else ""
                                for j in range(r.n_channels))
                out.write(f"{r.timestamps[i]:.0f},{vals}\n")
        else:
            for i in range(r.n_snapshots):
                vals = ",".join(
                    f"{r.data[i,j]:.4f}" if not np.isnan(r.data[i, j]) else ""
                    for j in range(r.n_channels))
                out.write(f"{r.timestamps[i]:.0f},{vals}\n")
    else:
        channels = args.channel or []
        if not channels:
            print("Specify --channel NAME or --all", file=sys.stderr)
            return
        out.write("epoch_ms,rel_ms,channel,kind,value\n")
        for ch_name in channels:
            info = r.resolve(ch_name)
            if info is None:
                print(f"Warning: '{ch_name}' not found, skipping",
                      file=sys.stderr)
                continue
            kind, idx = info
            if kind == "hv":
                col = r.data[:, idx]
                if args.absolute:
                    col = col + r.v0set_trace(idx)
                    row_kind = "vmon"
                else:
                    row_kind = "dv"
                ts = r.timestamps
            else:
                if args.imon:
                    src = r.booster_imon
                    row_kind = "imon"
                else:
                    src = r.booster_vmon
                    row_kind = "vmon"
                col = src[:, idx]
                ts  = r.booster_timestamps
            if len(ts) == 0:
                continue
            t0 = ts[0]
            for i in range(len(ts)):
                v = col[i]
                if not np.isnan(v):
                    out.write(f"{ts[i]:.0f},{ts[i]-t0:.0f},"
                              f"{ch_name},{row_kind},{v:.4f}\n")

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

    channels = args.channel or []
    if not channels:
        print("Specify --channel NAME [NAME ...]", file=sys.stderr)
        return

    fig, ax = plt.subplots(figsize=(12, 5))

    any_hv = any_bst = False
    for ch_name in channels:
        info = r.resolve(ch_name)
        if info is None:
            print(f"Warning: '{ch_name}' not found", file=sys.stderr)
            continue
        kind, idx = info
        if kind == "hv":
            col = r.data[:, idx]
            if args.absolute:
                col = col + r.v0set_trace(idx)
            ts = r.timestamps
            any_hv = True
        else:
            src = r.booster_imon if args.imon else r.booster_vmon
            col = src[:, idx]
            ts  = r.booster_timestamps
            any_bst = True
        if len(ts) == 0:
            continue
        t0 = ts[0]
        rel_s = (ts - t0) / 1000.0
        mask = ~np.isnan(col)
        ax.plot(rel_s[mask], col[mask], label=ch_name, linewidth=0.7)

    ax.set_xlabel("Time [s]")
    if any_bst and not any_hv:
        ax.set_ylabel("IMon [A]" if args.imon else "VMon [V]")
    elif any_hv and not any_bst:
        ax.set_ylabel("VMon [V]" if args.absolute else "dV = VMon - V0Set [V]")
    else:
        ax.set_ylabel("[V] or [A]")
    ax.legend()
    ax.grid(True, alpha=0.3)
    start_str = r.t0_datetime().strftime("%Y-%m-%d %H:%M:%S UTC")
    ax.set_title(f"VMDF — start {start_str}")
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
        description="Read VMDF v2 binary files produced by prad2hvd",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    sub = ap.add_subparsers(dest="command", required=True)

    p = sub.add_parser("summary", help="Print file summary")
    p.add_argument("files", nargs="+", help="VMDF .dat files")

    p = sub.add_parser("channels",
                       help="List HV channel names (+V0Set) and boosters (+VSet/ISet)")
    p.add_argument("files", nargs="+")

    p = sub.add_parser("stats", help="Per-channel statistics")
    p.add_argument("files", nargs="+")
    p.add_argument("--absolute", action="store_true",
                   help="HV stats on reconstructed VMon instead of dV "
                        "(boosters are always on VMon/IMon)")

    p = sub.add_parser("csv", help="Export to CSV")
    p.add_argument("files", nargs="+")
    p.add_argument("--channel", "-c", nargs="*",
                   help="Channel name(s) — HV or booster")
    p.add_argument("--all", "-a", action="store_true",
                   help="Export all HV channels (wide format, boosters skipped)")
    p.add_argument("--absolute", action="store_true",
                   help="Export reconstructed HV VMon instead of dV")
    p.add_argument("--imon", action="store_true",
                   help="For booster rows, use IMon instead of VMon")
    p.add_argument("--output", "-o", help="Output file (default: stdout)")

    p = sub.add_parser("plot", help="Plot channel traces")
    p.add_argument("files", nargs="+")
    p.add_argument("--channel", "-c", nargs="+", required=True,
                   help="Channel name(s) — HV or booster")
    p.add_argument("--absolute", action="store_true",
                   help="Plot reconstructed HV VMon instead of dV")
    p.add_argument("--imon", action="store_true",
                   help="For booster rows, plot IMon instead of VMon")
    p.add_argument("--output", "-o",
                   help="Save plot to file instead of showing")

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
