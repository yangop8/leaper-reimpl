#!/usr/bin/env python3
"""Plot the Figure-1-style view of the cache invalidation problem.

Usage:
  python3 tools/plot_timeseries.py experiments/results/m0_baseline [more_prefixes...]

Reads <prefix>.timeseries.csv and <prefix>.events.csv and writes <prefix>.png:
  panel 1  block cache hit ratio, with compaction/flush windows shaded
  panel 2  QPS and read p99 latency
  panel 3  resident cache bytes, split into reachable and stale (blocks whose
           SST was compacted away and can never be looked up again)
"""

import csv
import os
import sys
from collections import defaultdict

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt


def read_csv(path):
    with open(path, newline="") as f:
        rows = list(csv.DictReader(f))
    cols = defaultdict(list)
    for r in rows:
        for k, v in r.items():
            try:
                cols[k].append(float(v))
            except (TypeError, ValueError):
                cols[k].append(v)
    return cols


def read_events(path, ts_path=None):
    # Guard against an aborted run's leftover event file being paired with a
    # fresh timeseries: events.csv is written only at the end of a run, so it
    # must be at least as new as the timeseries it is supposed to describe.
    if ts_path and os.path.exists(path) and os.path.exists(ts_path):
        if os.path.getmtime(path) < os.path.getmtime(ts_path) - 1:
            raise SystemExit(
                f"{path} is older than {ts_path}: the event timeline is from an "
                f"earlier (probably aborted) run. Re-run or delete it.")
    spans, flushes = [], []
    open_compactions = []
    try:
        with open(path, newline="") as f:
            for r in csv.DictReader(f):
                t, typ = float(r["t"]), r["type"]
                if typ == "compaction_begin":
                    open_compactions.append(t)
                elif typ == "compaction_end" and open_compactions:
                    spans.append((open_compactions.pop(0), t))
                elif typ == "flush_end":
                    flushes.append(t)
    except FileNotFoundError:
        pass
    return spans, flushes


def plot(prefix):
    ts = read_csv(prefix + ".timeseries.csv")
    spans, flushes = read_events(prefix + ".events.csv", prefix + ".timeseries.csv")
    t = ts["t"]
    n = len(t)
    tmax = max(t) if t else 0

    fig, axes = plt.subplots(3, 1, figsize=(11, 8.5), sharex=True)

    # LevelDB schedules seek-triggered compactions on top of write-triggered
    # ones, so under read pressure a compaction is in flight in essentially
    # every second and shading the busy windows shades everything. Plot
    # compaction intensity as an underlay instead, so the anti-correlation with
    # hit ratio is visible.
    busy_frac = sum(1 for x in ts["compactions_running"] if x > 0) / n if n else 0

    ax = axes[0]
    axb = ax.twinx()
    bars = axb.bar(t, ts["compactions_done"], width=1.0, color="tab:red", alpha=0.20,
                   label="compactions completed")
    axb.set_ylabel("compactions / s")
    axb.set_zorder(0)
    ax.set_zorder(1)
    ax.patch.set_visible(False)
    line_hit, = ax.plot(t, [100.0 * x for x in ts["hit_ratio"]], color="tab:blue",
                        lw=1.2, label="block cache hit ratio")
    ax.set_ylabel("block cache\nhit ratio (%)")
    ax.grid(alpha=0.3)
    ax.legend(handles=[line_hit, bars], loc="lower left", fontsize=8)
    ax.set_title(
        f"{prefix}: cache invalidation on stock LevelDB 1.23\n"
        f"a compaction was in flight in {100.0 * busy_frac:.0f}% of seconds"
    )

    ax = axes[1]
    line_qps, = ax.plot(t, ts["qps"], color="tab:green", lw=1.2, label="QPS")
    ax.set_ylabel("QPS")
    ax.grid(alpha=0.3)
    ax2 = ax.twinx()
    line_p99, = ax2.plot(t, [x / 1000.0 for x in ts["read_p99_us"]], color="tab:red",
                         lw=1.0, label="read p99")
    ax2.set_ylabel("read p99 (ms)")
    ax.legend(handles=[line_qps, line_p99], loc="lower left", fontsize=8)

    ax = axes[2]
    reachable = [a - b for a, b in zip(ts["cache_live_mb"], ts["cache_stale_mb"])]
    ax.stackplot(t, reachable, ts["cache_stale_mb"],
                 labels=["reachable", "stale (SST compacted away)"],
                 colors=["tab:blue", "tab:gray"], alpha=0.75)
    ax.set_ylabel("resident cache (MB)")
    ax.set_xlabel("time (s)")
    ax.grid(alpha=0.3)
    axc = ax.twinx()
    churn, = axc.plot(t, [x * 4.0 / 1024.0 for x in ts["block_lookups"]],
                      color="tab:orange", lw=0.9, label="block reads (MB/s)")
    axc.set_ylabel("block reads (MB/s)")
    handles = ax.get_legend_handles_labels()[0] + [churn]
    ax.legend(handles=handles, loc="center right", fontsize=8)

    fig.tight_layout()
    out = prefix + ".png"
    fig.savefig(out, dpi=140)
    plt.close(fig)
    print("wrote", out)

    if n:
        busy = [i for i in range(n) if ts["compactions_running"][i] > 0]
        idle = [i for i in range(n) if ts["compactions_running"][i] == 0]

        def avg(idx, col):
            return sum(ts[col][i] for i in idx) / len(idx) if idx else float("nan")

        print(f"  seconds with a compaction running : {len(busy)}/{n}")
        print(f"  hit ratio   idle={avg(idle,'hit_ratio'):.4f}  busy={avg(busy,'hit_ratio'):.4f}")
        print(f"  QPS         idle={avg(idle,'qps'):.0f}  busy={avg(busy,'qps'):.0f}")
        print(f"  read p99us  idle={avg(idle,'read_p99_us'):.0f}  busy={avg(busy,'read_p99_us'):.0f}")
        print(f"  stale MB    mean={sum(ts['cache_stale_mb'])/n:.1f}  max={max(ts['cache_stale_mb']):.1f}"
              f"  (resident mean {sum(ts['cache_live_mb'])/n:.1f} MB)")

        # LevelDB schedules seek-triggered compactions, so a read-heavy run may
        # never have a compaction-free second and the idle/busy split above can
        # be empty. Quantify the effect against compaction intensity instead.
        def pearson(xs, ys):
            m = len(xs)
            mx, my = sum(xs) / m, sum(ys) / m
            cov = sum((a - mx) * (b - my) for a, b in zip(xs, ys))
            vx = sum((a - mx) ** 2 for a in xs) ** 0.5
            vy = sum((b - my) ** 2 for b in ys) ** 0.5
            return cov / (vx * vy) if vx and vy else float("nan")

        intensity = ts["compactions_done"]
        for col in ("hit_ratio", "qps", "read_p99_us"):
            print(f"  corr(compactions_done, {col}) = {pearson(intensity, ts[col]):+.3f}")

        hi = sorted(range(n), key=lambda i: -intensity[i])[: max(1, n // 5)]
        lo = sorted(range(n), key=lambda i: intensity[i])[: max(1, n // 5)]
        print(f"  top-20% compaction seconds : hit={avg(hi,'hit_ratio'):.4f} "
              f"qps={avg(hi,'qps'):.0f} p99={avg(hi,'read_p99_us'):.0f}us")
        print(f"  bottom-20% compaction secs : hit={avg(lo,'hit_ratio'):.4f} "
              f"qps={avg(lo,'qps'):.0f} p99={avg(lo,'read_p99_us'):.0f}us")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__)
        raise SystemExit(2)
    for p in sys.argv[1:]:
        plot(p)
