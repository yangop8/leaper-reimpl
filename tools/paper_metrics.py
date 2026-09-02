#!/usr/bin/env python3
"""Report a policy matrix in the paper's own terms.

The paper's Table 4 does not report a whole-run hit ratio. It reports, for the
windows "during and after compactions":

    QPS improvement, latency reduction, and cache misses eliminated

and its headline "eliminates about 70% cache invalidations and 99% of latency
spikes with at most 0.95% overheads" rests on three quantities this script
computes from the per-second CSVs and the event timelines:

  invalidation   |C ∩ M_i|: blocks resident in the cache when a compaction
                 started, summed (needs the invalidated_blocks column, i.e. a
                 run with the plug-in attached); plus misses inside the
                 compaction windows [begin, end + T2]
  spikes         seconds whose read p95 exceeds k times the run's median p95
  overhead       QPS *outside* compaction windows, relative to the stock run --
                 only meaningful for unthrottled runs (op_rate=0)

Usage:
  python3 tools/paper_metrics.py experiments/results m4_ovh --t2=2 --k=2
"""

import argparse
import csv
import os
import statistics

POLICIES = ["off", "eager_evict", "incremental_warmup", "warm_all", "warm_flush",
            "leaper", "leaper_p2only", "leaper_p2only_ssad", "oracle"]


def read_ts(path):
    with open(path, newline="") as f:
        return list(csv.DictReader(f))


def read_windows(path, t2):
    spans, opened = [], []
    if not os.path.exists(path):
        return spans
    with open(path, newline="") as f:
        for r in csv.DictReader(f):
            t, typ = float(r["t"]), r["type"]
            if typ in ("compaction_begin", "flush_begin"):
                opened.append(t)
            elif typ in ("compaction_end", "flush_end") and opened:
                spans.append((opened.pop(0), t + t2))
    return spans


def in_window(t, spans):
    return any(a <= t <= b for a, b in spans)


def summarise(prefix, t2, k):
    ts_path = prefix + ".timeseries.csv"
    if not os.path.exists(ts_path):
        return None
    ts = read_ts(ts_path)
    spans = read_windows(prefix + ".events.csv", t2)
    p95 = [float(r["read_p95_us"]) for r in ts]
    med = statistics.median(p95) if p95 else 0.0
    out = {"secs": len(ts), "win_secs": 0, "qps_out": [], "qps_in": [],
           "miss_in": 0, "miss_all": 0, "look_in": 0, "look_all": 0,
           "miss_out": 0, "look_out": 0,
           "spikes": sum(1 for v in p95 if med and v > k * med),
           "inval": 0, "p99_all": [], "pf_prec": None, "pf_inserted": 0}
    for r in ts:
        t = float(r["t"])
        look = float(r["block_lookups"]); hit = float(r["block_hits"])
        miss = look - hit
        out["miss_all"] += miss; out["look_all"] += look
        out["p99_all"].append(float(r["read_p99_us"]))
        if in_window(t, spans):
            out["win_secs"] += 1
            out["qps_in"].append(float(r["qps"]))
            out["miss_in"] += miss; out["look_in"] += look
        else:
            out["qps_out"].append(float(r["qps"]))
            out["miss_out"] += miss; out["look_out"] += look
        if "invalidated_blocks" in r:
            out["inval"] += float(r["invalidated_blocks"] or 0)
    # The pf_* counters are cumulative since process start and so include
    # blocks warmed while the database was being filled and during warmup;
    # those are never read in the measured window and would deflate the
    # precision (WarmAll: 111k of 406k on one run). Use the measured window's
    # own deltas, taking the first row as the baseline.
    first, last = ts[0], ts[-1]
    def delta(k):
        return float(last.get(k) or 0) - float(first.get(k) or 0)
    if "pf_inserted" in last and delta("pf_inserted") > 0:
        out["pf_inserted"] = delta("pf_inserted")
        out["pf_prec"] = delta("pf_read_once") / delta("pf_inserted")
    elif "pf_evicted" in last and delta("pf_evicted") > 0:
        out["pf_prec"] = delta("pf_used") / delta("pf_evicted")
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out_dir")
    ap.add_argument("tag")
    ap.add_argument("--t2", type=float, default=2.0, help="recovery window after each op, s")
    ap.add_argument("--k", type=float, default=2.0, help="spike = p95 > k * median p95")
    args = ap.parse_args()

    rows = {}
    for p in POLICIES:
        r = summarise(os.path.join(args.out_dir, f"{args.tag}_{p}"), args.t2, args.k)
        if r:
            rows[p] = r
    if "off" not in rows:
        raise SystemExit("no stock run to compare against")
    base = rows["off"]
    b_qps_out = statistics.mean(base["qps_out"]) if base["qps_out"] else float("nan")
    # Each policy is a separate run with its own compaction timing, so absolute
    # miss counts inside windows are not comparable across policies; the miss
    # *rate* inside windows is.
    b_rate_in = base["miss_in"] / base["look_in"] if base["look_in"] else float("nan")

    print(f"\n{args.tag}: windows = [op begin, op end + {args.t2:.0f}s], "
          f"{base['win_secs']}/{base['secs']} s inside windows for the stock run\n")
    print(f"{'policy':<20} {'miss rate win':>14} {'vs stock':>9} {'hit in win':>11} {'hit out':>8} "
          f"{'spikes':>7} {'QPS out':>9} {'overhead':>9} {'inval blk':>10} {'warmed':>8} {'pf prec':>8}")
    print("-" * 124)
    for p, r in rows.items():
        qps_out = statistics.mean(r["qps_out"]) if r["qps_out"] else float("nan")
        rate_in = r["miss_in"] / r["look_in"] if r["look_in"] else float("nan")
        d_miss = 100.0 * (1 - rate_in / b_rate_in) if b_rate_in else float("nan")
        hit_in = 100.0 * (1 - rate_in)
        hit_out = 100.0 * (1 - r["miss_out"] / r["look_out"]) if r["look_out"] else float("nan")
        ovh = 100.0 * (qps_out / b_qps_out - 1) if b_qps_out == b_qps_out and b_qps_out else float("nan")
        pf = f"{r['pf_prec']:.3f}" if r["pf_prec"] is not None else "-"
        print(f"{p:<20} {100 * rate_in:>13.2f}% {d_miss:>+8.1f}% {hit_in:>10.2f}% {hit_out:>7.2f}% "
              f"{r['spikes']:>7d} {qps_out:>9,.0f} {ovh:>+8.2f}% {r['inval']:>10,.0f} "
              f"{r['pf_inserted']:>8,.0f} {pf:>8}")
    print()
    print("miss rate win / vs stock : the paper's 'cache misses eliminated' (Table 4), as a rate")
    print("hit out                  : hit ratio outside the windows -- where prefetched blocks pay off")
    print("spikes                   : the paper's 'latency spikes' (Fig. 1), p95 > k x median")
    print("QPS out / overhead       : the paper's overhead measure -- QPS outside background-op windows,")
    print("                           relative to stock. Only meaningful for unthrottled runs.")
    print("inval blk                : |C ∩ M_i| summed over compactions (Formulation 2)")
    print("warmed / pf prec         : blocks prefetched in the measured window, and the share of them read")
    print("                           at least once (resident or evicted)")


if __name__ == "__main__":
    main()
