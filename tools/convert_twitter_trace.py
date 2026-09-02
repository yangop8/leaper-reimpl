#!/usr/bin/env python3
"""Convert a Twitter cache-trace (twitter/cache-trace, 2020Mar) into the
8-byte access-trace format the Leaper pipeline consumes.

Input rows: timestamp,anonymized_key,key_size,value_size,client_id,operation,ttl

Why re-key. Leaper's key ranges have to be *contiguous in the storage engine's
byte order*, because that is what a compaction rewrites together. Twitter keys
are anonymised strings, so the only order that exists is the byte order an LSM
would store them in. Each distinct key is therefore assigned its rank in the
byte-sorted key set and stored under that rank as a 16-digit decimal, which is
exactly how the synthetic harness keys look. Namespace locality survives
(keys sharing a namespace prefix sort together), and the decimal RangeMapper
then works unchanged.

Operation mapping: get/gets -> read (0); every mutating op -> update (1). There
is no separate insert class: a cache trace does not distinguish a first write
from a later one, and for the storage engine both are a Put.

Usage:
  python3 tools/convert_twitter_trace.py cluster019.csv --out experiments/results/tw19
Produces <out>.trace.0 and <out>.meta.
"""

import argparse
import struct
import sys

import numpy as np

READ_OPS = {"get", "gets"}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("csv")
    ap.add_argument("--out", required=True)
    ap.add_argument("--max_rows", type=int, default=0, help="0 = all")
    args = ap.parse_args()

    ts, keys, ops = [], [], []
    n_skipped = 0
    with open(args.csv, "r", errors="replace") as f:
        for i, line in enumerate(f):
            if args.max_rows and i >= args.max_rows:
                break
            parts = line.rstrip("\n").split(",")
            if len(parts) < 6:
                n_skipped += 1
                continue
            try:
                t = int(parts[0])
            except ValueError:
                n_skipped += 1
                continue
            ts.append(t)
            keys.append(parts[1])
            ops.append(0 if parts[5] in READ_OPS else 1)
    if not ts:
        sys.exit("no rows parsed")

    ts = np.asarray(ts, dtype=np.int64)
    ops = np.asarray(ops, dtype=np.int8)
    t0 = ts.min()
    t_ms = (ts - t0) * 1000  # trace timestamps are whole seconds
    if t_ms.max() >= 2**32:
        sys.exit("trace longer than uint32 ms; split it")

    # Rank keys in byte order.
    uniq, inverse = np.unique(np.asarray(keys), return_inverse=True)
    rank = inverse.astype(np.int64)
    if len(uniq) >= 2**30:
        sys.exit("more than 2^30 distinct keys; the packed record cannot hold them")

    packed = (rank.astype(np.uint32) << 2) | ops.astype(np.uint32)
    out = np.empty(len(ts) * 2, dtype=np.uint32)
    out[0::2] = t_ms.astype(np.uint32)
    out[1::2] = packed
    out.tofile(args.out + ".trace.0")

    span = (ts.max() - t0)
    reads = int((ops == 0).sum())
    with open(args.out + ".meta", "w") as m:
        m.write("# converted from twitter/cache-trace; op 0=get/gets 1=any write\n")
        m.write(f"source={args.csv}\nthreads=1\nnum_keys={len(uniq)}\n")
        m.write(f"rows={len(ts)}\nspan_s={span}\nread_ratio={reads / len(ts):.4f}\n")
        m.write("key_dist=twitter\nclock_offset_s=0\n")
    print(f"{args.csv}: {len(ts):,} rows over {span:,}s, {len(uniq):,} distinct keys, "
          f"read share {reads / len(ts):.3f}, skipped {n_skipped}")
    print(f"wrote {args.out}.trace.0 and {args.out}.meta")
    # Per-key ranking table so the replay harness can load the DB with the
    # same key -> rank mapping (rank r is stored as key %016d).
    with open(args.out + ".keys.txt", "w") as k:
        for r, key in enumerate(uniq):
            k.write(f"{r}\t{key}\n")


if __name__ == "__main__":
    main()
