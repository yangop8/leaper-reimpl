#!/usr/bin/env python3
"""Build the oracle's per-interval hot sets from a captured trace.

The oracle is the upper bound the learned policy is measured against: it is
given the ranges that will actually be read in the next interval. Without it,
"Leaper improved the hit ratio by X" has no scale -- X could be most of what is
achievable or a tenth of it.

The trace's clock starts at the measurement window; the plug-in's clock starts
at the run, so --slot_offset shifts by the warmup.

Usage:
  python3 tools/make_oracle.py --trace=<prefix> --range_size=40000 \
      --slot_s=1 --slot_offset=30 --out=<prefix>.oracle.txt
"""

import argparse
import glob
import sys

import numpy as np


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--trace", required=True)
    ap.add_argument("--range_size", type=int, required=True)
    ap.add_argument("--slot_s", type=float, default=1.0)
    ap.add_argument("--slot_offset", type=int, default=0,
                    help="intervals to add so trace time matches plug-in time "
                         "(normally warmup / slot_s)")
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    files = sorted(glob.glob(args.trace + ".trace.*"))
    if not files:
        sys.exit(f"no trace files matching {args.trace}.trace.*")
    t_ms_all, key_all, op_all = [], [], []
    for f in files:
        raw = np.fromfile(f, dtype=np.uint32)
        if raw.size % 2:
            raw = raw[:-1]
        raw = raw.reshape(-1, 2)
        t_ms_all.append(raw[:, 0].astype(np.int64))
        key_all.append((raw[:, 1] >> 2).astype(np.int64))
        op_all.append((raw[:, 1] & 3).astype(np.int8))
    t_ms = np.concatenate(t_ms_all)
    key = np.concatenate(key_all)
    op = np.concatenate(op_all)

    is_read = op == 0
    slot = (t_ms[is_read] // int(args.slot_s * 1000)).astype(np.int64) + args.slot_offset
    rid = key[is_read] // args.range_size

    order = np.lexsort((rid, slot))
    slot, rid = slot[order], rid[order]
    keep = np.ones(len(slot), dtype=bool)
    keep[1:] = (slot[1:] != slot[:-1]) | (rid[1:] != rid[:-1])
    slot, rid = slot[keep], rid[keep]

    with open(args.out, "w") as f:
        f.write("# slot range... : ranges actually read in that interval\n")
        start = 0
        while start < len(slot):
            end = start
            while end < len(slot) and slot[end] == slot[start]:
                end += 1
            f.write(f"{slot[start]} " + " ".join(str(int(r)) for r in rid[start:end]) + "\n")
            start = end
    print(f"wrote {args.out}: {len(np.unique(slot))} intervals, "
          f"{len(slot)} (interval, range) pairs")


if __name__ == "__main__":
    main()
