#!/usr/bin/env python3
"""Does Algorithm 2 actually recover a planted precursor relation?

The lifecycle workload generator plants a known relation: within one chain,
range f(A) goes hot shortly after range A, where f is a fixed pseudo-random
successor function. This script replays the generator's hash in Python to
recover the ground-truth pairs, runs Algorithm 2 on the captured trace at the
generator's own granularity, and reports how often the true precursor is among
the gamma the algorithm selected.

Without this, "the precursor features contributed X" is unfalsifiable: a gain
could come from the relation being found, or from an unrelated correlation.

Usage:
  python3 tools/check_precursors.py --trace=experiments/results/m1_c4
"""

import argparse
import sys

import numpy as np

import train_leaper as tl

MASK = (1 << 64) - 1


def mix(seed, a, b):
    """Port of LifecycleChooser::Mix (bench/include/leaper_bench/keygen.h)."""
    h = (seed ^ ((a * 0x9E3779B97F4A7C15) & MASK) ^ ((b * 0xC2B2AE3D27D4EB4F) & MASK)) & MASK
    h ^= h >> 33
    h = (h * 0xFF51AFD7ED558CCD) & MASK
    h ^= h >> 33
    h = (h * 0xC4CEB9FE1A85EC53) & MASK
    h ^= h >> 33
    return h


def successor(seed, r, n_ranges):
    return mix(seed ^ 0xA5A5A5A5A5A5A5A5, r, 0) % n_ranges


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--trace", required=True)
    ap.add_argument("--slot_s", type=float, default=1.0)
    ap.add_argument("--gamma", type=int, default=3)
    ap.add_argument("--eps", type=float, default=0.5)
    ap.add_argument("--train_frac", type=float, default=0.75)
    ap.add_argument("--valid_frac", type=float, default=0.8)
    args = ap.parse_args()

    meta = tl.load_meta(args.trace)
    need = ["life_range_size", "num_keys", "seed", "life_chain"]
    missing = [k for k in need if k not in meta]
    if missing:
        sys.exit(f"trace meta is missing {missing}; was it captured with key_dist=lifecycle?")
    range_size = int(meta["life_range_size"])
    num_keys = int(meta["num_keys"])
    seed = int(meta["seed"])
    chain = int(meta["life_chain"])
    n_ranges = num_keys // range_size
    print(f"[truth] generator: {n_ranges} ranges of {range_size} keys, chain={chain}, seed={seed}")
    if chain <= 1:
        print("[truth] chain=1: no precursor relation was planted. Anything Algorithm 2 "
              "reports here is coincidence, which is the point of running the control.")

    t_ms, key, op, per_thread = tl.load_trace(args.trace)
    slot = (t_ms // int(args.slot_s * 1000)).astype(np.int64)
    n_slots = int(slot.max()) + 1
    train_slot = int(n_slots * args.train_frac * args.valid_frac)
    train_until_ms = int((train_slot + 1) * args.slot_s * 1000)
    print(f"[truth] {n_slots} slots; precursors fitted on slots <= {train_slot}")

    rid = key // range_size
    keep = rid < n_ranges
    read_rate = np.zeros((n_ranges, n_slots), dtype=np.float32)
    is_read = (op == 0) & keep
    np.add.at(read_rate, (rid[is_read], slot[is_read]), 1.0)

    precursors, transfer = tl.compute_precursors(
        per_thread, range_size, n_ranges, read_rate[:, : train_slot + 1],
        args.gamma, args.eps, train_until_ms)

    # Ground truth: the true precursor of target t is any r with f(r) == t.
    preimage = [[] for _ in range(n_ranges)]
    for r in range(n_ranges):
        preimage[successor(seed, r, n_ranges)].append(r)

    active = np.where(read_rate[:, : train_slot + 1].sum(axis=1) > 0)[0]
    have_pre = [t for t in active if precursors[t, 0] >= 0 and preimage[t]]
    if not have_pre:
        print("[truth] no target range has both a selected precursor and a true one")
        return

    hits = sum(1 for t in have_pre
               if set(precursors[t][precursors[t] >= 0].tolist()) & set(preimage[t]))
    # Chance level: gamma draws without replacement from the active ranges.
    exp_chance = np.mean([min(1.0, args.gamma * len(preimage[t]) / len(active))
                          for t in have_pre])

    print(f"[truth] {len(active)} ranges active in the training window")
    print(f"[truth] {len(have_pre)} targets have both a selected and a true precursor")
    print(f"[truth] Algorithm 2 recall@gamma={args.gamma}: {hits}/{len(have_pre)} = "
          f"{hits / len(have_pre):.3f}   (chance = {exp_chance:.3f})")

    # Is the relation even visible in the raw transfer counts, before the
    # cosine filter? If it is here but not above, the filter is what loses it.
    top = np.argsort(-transfer, axis=1)[:, : args.gamma]
    thits = sum(1 for t in have_pre if set(top[t].tolist()) & set(preimage[t]))
    print(f"[truth] top-{args.gamma} by transfer count alone: {thits}/{len(have_pre)} = "
          f"{thits / len(have_pre):.3f}")


if __name__ == "__main__":
    main()
