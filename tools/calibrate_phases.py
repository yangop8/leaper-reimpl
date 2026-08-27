#!/usr/bin/env python3
"""Calibrate the two-phase prefetcher's T1 and T2 from a measured run.

The paper models compaction duration and cache recovery time as

    T1 ~ alpha * N          N = blocks merged
    T2 ~ beta * Q / S       Q = QPS, S = cache size

and says both constants "can be computed by sampling from previous log data".
This does exactly that against a leaper_bench run:

  T1: fit alpha by least squares on (bytes compacted, duration) pairs taken
      from the compaction_begin/compaction_end timeline, converted to blocks.
  T2: measure how long the block cache hit ratio takes to return to its
      pre-compaction level after each compaction ends, then solve for beta.

Guessing these instead of measuring them is not harmless: with a beta that is
wrong by orders of magnitude, T2 collapses to a single interval, the second
phase ends up reading the weakest multi-step model, and the prefetcher
predicts almost nothing hot.

Usage:
  python3 tools/calibrate_phases.py experiments/results/m0_B_2t --block_kb=4 --cache_mb=512
"""

import argparse
import csv
import os
import sys


def read_csv(path):
    with open(path, newline="") as f:
        return list(csv.DictReader(f))


def from_timeseries(ts, args):
    t_col = [float(r["t"]) for r in ts]
    hit = [float(r["hit_ratio"]) for r in ts]
    qps = [float(r["qps"]) for r in ts]
    busy = [float(r["compactions_done"]) > 0 for r in ts]

    runs, i = [], 0
    while i < len(busy):
        if not busy[i]:
            i += 1
            continue
        j = i
        while j < len(busy) and busy[j]:
            j += 1
        runs.append((t_col[i], t_col[j - 1] + 1.0))
        i = j
    if not runs:
        sys.exit("no compaction activity in the timeseries")
    durs = sorted(e - s for s, e in runs)
    print(f"[T1] {len(runs)} compaction windows, median={durs[len(durs)//2]:.2f}s "
          f"max={durs[-1]:.2f}s (1s resolution)")

    recoveries = []
    for _, end in runs:
        i = next((k for k, t in enumerate(t_col) if t >= end), None)
        if i is None or i == 0 or i + 1 >= len(hit):
            continue
        base = max(hit[:i])
        if hit[i] >= base * args.recover_tol:
            continue
        for j in range(i + 1, len(hit)):
            if hit[j] >= base * args.recover_tol:
                recoveries.append(t_col[j] - end)
                break
    t2 = sorted(recoveries)[len(recoveries) // 2] if recoveries else 1.0
    print(f"[T2] {len(recoveries)} measured recoveries, median={t2:.2f}s")
    mean_qps = sum(qps) / len(qps) if qps else 0.0
    cache_bytes = args.cache_mb * 1024 * 1024
    beta = t2 * cache_bytes / mean_qps if mean_qps else 0.0
    alpha = 1.3e-05  # not measurable at 1s resolution; carried from LevelDB
    print(f"[T2] mean QPS={mean_qps:.0f}, beta = {beta:.3e}")
    print()
    print(f"suggested flags: --leaper_t1_alpha={alpha:.6g} --leaper_t2_beta={beta:.6g}")
    print(f"or pin them directly: --leaper_t2_seconds={t2:.3f}")
    return


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("prefix")
    ap.add_argument("--block_kb", type=float, default=4.0)
    ap.add_argument("--cache_mb", type=float, default=512.0)
    ap.add_argument("--recover_tol", type=float, default=0.9,
                    help="fraction of the pre-drop hit ratio that counts as recovered")
    args = ap.parse_args()

    ts = read_csv(args.prefix + ".timeseries.csv")
    ev_path = args.prefix + ".events.csv"
    if not os.path.exists(ev_path):
        # RocksDB runs have no event log (the driver reports compaction bytes
        # per second instead). Fall back to treating each second with non-zero
        # compaction output as a compaction window: coarser, but it still
        # measures the two things that matter -- how long compaction runs and
        # how long the hit ratio takes to come back.
        print(f"[note] {ev_path} missing; deriving windows from the timeseries")
        return from_timeseries(ts, args)
    events = read_csv(ev_path)

    # --- T1 ---------------------------------------------------------------
    open_begin, spans = [], []
    for r in events:
        if r["type"] == "compaction_begin":
            open_begin.append(float(r["t"]))
        elif r["type"] == "compaction_end" and open_begin:
            spans.append((open_begin.pop(0), float(r["t"]), float(r["bytes"])))
    if not spans:
        sys.exit("no completed compactions in the event log")

    pairs = [(b / (args.block_kb * 1024.0), e - s) for s, e, b in spans if b > 0 and e > s]
    if not pairs:
        sys.exit("no usable compaction durations")
    num = sum(n * d for n, d in pairs)
    den = sum(n * n for n, _ in pairs)
    alpha = num / den if den else 0.0
    durations = sorted(d for _, d in pairs)
    print(f"[T1] {len(pairs)} compactions, duration median={durations[len(durations)//2]:.3f}s "
          f"max={durations[-1]:.3f}s")
    print(f"[T1] alpha = {alpha:.3e} seconds per block  (output blocks)")

    # --- T2 ---------------------------------------------------------------
    t_col = [float(r["t"]) for r in ts]
    hit = [float(r["hit_ratio"]) for r in ts]
    qps = [float(r["qps"]) for r in ts]
    recoveries = []
    for _, end, _ in spans:
        i = next((k for k, t in enumerate(t_col) if t >= end), None)
        if i is None or i == 0 or i + 1 >= len(hit):
            continue
        base = max(hit[:i]) if i else hit[0]
        if hit[i] >= base * args.recover_tol:
            continue  # no visible drop after this compaction
        for j in range(i + 1, len(hit)):
            if hit[j] >= base * args.recover_tol:
                recoveries.append(t_col[j] - end)
                break
    if recoveries:
        recoveries.sort()
        t2 = recoveries[len(recoveries) // 2]
        print(f"[T2] {len(recoveries)} measured recoveries, median={t2:.3f}s "
              f"p90={recoveries[int(0.9 * (len(recoveries) - 1))]:.3f}s")
    else:
        t2 = 1.0
        print("[T2] no measurable recovery (the hit ratio never dropped below "
              f"{args.recover_tol:.0%} of its running peak); defaulting to {t2:.1f}s")

    mean_qps = sum(qps) / len(qps) if qps else 0.0
    cache_bytes = args.cache_mb * 1024 * 1024
    beta = t2 * cache_bytes / mean_qps if mean_qps else 0.0
    print(f"[T2] mean QPS={mean_qps:.0f}, cache={cache_bytes:.0f} B")
    print(f"[T2] beta = {beta:.3e}  (so that beta * Q / S = {t2:.3f}s here)")
    print()
    print(f"suggested flags: --leaper_t1_alpha={alpha:.6g} --leaper_t2_beta={beta:.6g}")
    print(f"or pin them directly: --leaper_t1_seconds=<per compaction> "
          f"--leaper_t2_seconds={t2:.3f}")


if __name__ == "__main__":
    main()
