#!/usr/bin/env python3
"""Multi-seed summary: mean, spread and paired margins across evaluation seeds.

    seed_stats.py OUT_DIR POLICIES TAG [TAG ...]

Each TAG is one evaluation seed's matrix (TAG_POLICY.timeseries.csv). The
first policy in the comma-separated POLICIES is the reference; margins are
paired per seed (same seed = same traffic for every policy), so a margin's
spread is the spread of per-seed differences, not of two means.
"""

import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from summarize_matrix import LABEL, load  # noqa: E402


def stats(xs):
    n = len(xs)
    if n == 0:
        return None
    m = sum(xs) / n
    sd = math.sqrt(sum((x - m) ** 2 for x in xs) / (n - 1)) if n > 1 else 0.0
    return m, sd, min(xs), max(xs), n


def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    out_dir, policies, tags = sys.argv[1], sys.argv[2].split(","), sys.argv[3:]
    rows = {}  # policy -> {tag: result}
    for p in policies:
        for t in tags:
            r = load(os.path.join(out_dir, f"{t}_{p}"))
            if r is not None:
                rows.setdefault(p, {})[t] = r
    ref = policies[0]
    print(f"\n{'policy':<26} {'seeds':>5} {'hit ratio mean':>14} {'sd':>6} {'min':>8} {'max':>8} "
          f"{'vs ' + LABEL.get(ref, ref):>22} {'sd':>6} {'min':>8} {'max':>8} {'QPS':>7} {'comps':>7}")
    print("-" * 132)
    for p in policies:
        got = rows.get(p, {})
        if not got:
            print(f"{LABEL.get(p, p):<26} {'(missing)':>5}")
            continue
        hit = stats([100 * r["hit_ratio"] for r in got.values()])
        qps = sum(r["qps"] for r in got.values()) / len(got)
        comps = sum(r["compactions"] for r in got.values()) / len(got)
        line = (f"{LABEL.get(p, p):<26} {hit[4]:>5} {hit[0]:>13.2f}% {hit[1]:>6.2f} {hit[2]:>7.2f}% {hit[3]:>7.2f}%")
        if p != ref:
            common = [t for t in got if t in rows.get(ref, {})]
            diffs = [100 * (got[t]["hit_ratio"] - rows[ref][t]["hit_ratio"]) for t in common]
            d = stats(diffs)
            if d:
                sign = "all same sign" if all(x > 0 for x in diffs) or all(x < 0 for x in diffs) else "MIXED SIGN"
                line += f" {d[0]:>+19.2f}pp {d[1]:>6.2f} {d[2]:>+7.2f} {d[3]:>+7.2f}  {qps:>7.0f} {comps:>7.0f}  {sign}"
            else:
                line += f" {'(no pair)':>22}"
        else:
            line += f" {'':>22} {'':>6} {'':>8} {'':>8} {qps:>7.0f} {comps:>7.0f}"
        print(line)
    print("\nseeds (tags):", " ".join(tags))
    print("hit ratio: workload-thread block cache hits / lookups over the measured window, per seed;")
    print("margins: per-seed paired differences against the first policy (mean, sample sd, min, max).")


if __name__ == "__main__":
    main()
