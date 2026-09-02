#!/usr/bin/env python3
"""Summarise a policy matrix into one comparable table."""

import csv
import os
import sys

POLICIES = ["off", "eager_evict", "incremental_warmup", "warm_all", "warm_flush",
            "flush_only", "flush_and_compaction", "leaper", "leaper_p2only",
            "leaper_p2only_ssad", "leaper_rowcache", "oracle"]
LABEL = {"off": "LRU (stock)", "eager_evict": "EagerEvict",
         "incremental_warmup": "IncrementalWarmup", "warm_all": "WarmAll",
         "warm_flush": "WarmFlushOnly",
         "leaper": "Leaper (both phases)",
         "leaper_p2only": "Leaper (prefetch only)", "oracle": "Oracle",
         "leaper_p2only_ssad": "Leaper (prefetch, SSAD)", "leaper_rowcache": "Leaper + row cache",
         "flush_only": "kFlushOnly", "flush_and_compaction": "kFlushAndCompaction"}


def load(prefix):
    path = prefix + ".timeseries.csv"
    if not os.path.exists(path):
        return None
    with open(path, newline="") as f:
        rows = list(csv.DictReader(f))
    if not rows:
        return None
    n = len(rows)
    g = lambda k: [float(r[k]) for r in rows]
    hits, looks = sum(g("block_hits")), sum(g("block_lookups"))
    return {
        "hit_ratio": hits / looks if looks else 0.0,
        "qps": sum(g("qps")) / n,
        "p99": sum(g("read_p99_us")) / n,
        "p95": sum(g("read_p95_us")) / n,
        "misses": looks - hits,
        "compactions": sum(g("compactions_done")),
        "secs": n,
    }


def main():
    out_dir, tag = sys.argv[1], sys.argv[2]
    base = None
    print(f"\n{'policy':<20} {'hit ratio':>10} {'vs LRU':>9} {'QPS':>9} "
          f"{'vs LRU':>8} {'p95 us':>8} {'p99 us':>8} {'misses':>12}")
    print("-" * 92)
    for p in POLICIES:
        r = load(os.path.join(out_dir, f"{tag}_{p}"))
        if r is None:
            print(f"{LABEL[p]:<20} {'(missing)':>10}")
            continue
        if base is None:
            base = r
        dh = 100.0 * (r["hit_ratio"] - base["hit_ratio"])
        dq = 100.0 * (r["qps"] / base["qps"] - 1.0) if base["qps"] else 0.0
        print(f"{LABEL[p]:<20} {100 * r['hit_ratio']:9.2f}% {dh:+8.2f}pp "
              f"{r['qps']:9.0f} {dq:+7.1f}% {r['p95']:8.0f} {r['p99']:8.0f} "
              f"{r['misses']:12,.0f}")
    print()


if __name__ == "__main__":
    main()
