# M5-M7 — Porting to RocksDB

> **Hit-ratio caveat (M8, 2026-09-02).** The hit ratios in this file come
> from RocksDB's process-wide `BLOCK_CACHE_DATA_*` tickers, which also count
> compaction input reads and the Leaper listener's own warming scans (the
> latter register as data misses against Leaper). The harness now counts the
> workload threads only via `PerfContext`; the corrected matrix is `m7v3` in
> `docs/M8-review-followup.md`, section H.

> **Scope note.** These measurements come from a clean-room reimplementation on
> LevelDB/RocksDB with synthetic workloads and NVMe (slow storage is emulated).
> The paper's results come from X-Engine, real Tmall and DingTalk traffic, and
> spinning disks. The numbers below describe *this* setup and are not a
> replication of the paper's. See the top of the repository README.

M5 is the claim that `leaper/` is engine-independent. The only way to test that
claim is to make a second engine work without touching it, which is what M6-M7
do: `adapters/rocksdb/leaper_rocksdb.cc` links the same `leaper_core` — same
collector, same LightGBM scorer, same multi-step prediction, same two-phase
policy — with **no change to any file under `leaper/`**.

## The port is smaller than the LevelDB one, for one reason

LevelDB needed a 277-line patch because it has no way to observe compaction and
no way to address a block cache entry from outside. RocksDB ships
`EventListener`, so **the RocksDB integration needs no core patch at all**. That
asymmetry is the honest summary of the two engines' extensibility, and it is
worth stating plainly rather than presenting both ports as equivalent work.

| | LevelDB | RocksDB |
|---|---|---|
| compaction visibility | patch (`LeaperHooks`) | `EventListener`, built in |
| per-output-block metadata | patch (`TableBuilder::Flush`) | not exposed |
| block cache entry addressing | patch (`Table::block_cache_id`) | not exposed |
| read-path hook | patch (`DBImpl::Get`) | driven from the client |
| block cache bypassed by default | **yes** (mmap; see M0 finding 1) | no (`allow_mmap_reads` is false) |
| core patch size | 277 lines / 8 files | **zero** |

## What the RocksDB port cannot do, and why

**No phase 1 (eviction).** Leaper's eviction phase has to drop individual block
cache entries. RocksDB derives those keys from a per-file `OffsetableCacheKey`
held inside the table reader and does not expose it, so eviction is not
implementable as a plug-in. Leaper on RocksDB is therefore prefetch-only, and
the results are labelled as such rather than compared to the LevelDB numbers as
if they measured the same thing.

**Prefetch is at range granularity, not block granularity.** Without
per-output-block metadata, a predicted-hot range is warmed by seeking to its
first key and scanning forward with `fill_cache` on, bounded by
`--warm_scan_keys`. This pulls in exactly the blocks covering that range,
including blocks of files the compaction did not write. That is a deviation
from the paper — which intersects hot ranges with the boundaries of the blocks
being written (Algorithm 3) — and it cuts both ways: it warms data the
compaction did not touch (extra work), but it warms what will actually be read
(the point of the exercise).

**The collector runs in the client.** RocksDB has no read hook. In a deployment
this belongs in `DBImpl::GetImpl`; here it stays out of tree so the engine is
stock. It changes nothing about what is measured — the same keys, at the same
times — but it does mean the collector's cost is not inside the engine's
measured latency.

## What M7 compares, and why it is not the same question as M4

RocksDB already ships the prefetch side, unconditionally:

* `prepopulate_block_cache = kFlushOnly` — warm blocks written by flushes
* `kFlushAndCompaction` — warm blocks written by flushes *and* compactions,
  inserted at BOTTOM priority (added after v9; present in v11.8, which is what
  is pinned here — v9.10 has only the first two)

So on RocksDB the question is not "can the cache be warmed after a background
operation" but **"does choosing what to warm beat warming everything"**. Leaper
has to show equal or better hit ratio while inserting fewer blocks; matching
`kFlushAndCompaction`'s hit ratio at a fraction of its cache churn is a win, and
beating it on hit ratio would be a strong one. The matrix is therefore
off / kFlushOnly / kFlushAndCompaction / Leaper.

## Build

RocksDB is built out of tree so that a full RocksDB build does not dominate
configure time for anyone who only wants the LevelDB half:

```sh
cmake -S third_party/rocksdb -B build-rocksdb -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DWITH_GFLAGS=0 -DWITH_TESTS=0 -DWITH_TOOLS=0 -DROCKSDB_BUILD_SHARED=0
cmake --build build-rocksdb --target rocksdb -j
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
```

The M5-M7 targets appear only if `build-rocksdb/librocksdb.a` exists.

One build detail that is a real constraint rather than a preference: both
LevelDB and RocksDB are compiled `-fno-rtti`, so anything deriving from their
interfaces must match, while `leaper_core` keeps RTTI enabled so that
RTTI-enabled code can still derive from *its* interfaces. Getting this backwards
fails at link time with "typeinfo for rocksdb::Customizable" and similar.

## M7 results (NVMe, 4M keys, 128 MB cache, 40k ops/s, 4k writes/s, 300 s)

| policy | hit ratio | vs LRU | QPS | p95 | p99 |
|---|---|---|---|---|---|
| kDisable (stock) | 81.42% | — | 40,181 | 10 us | 19 us |
| kFlushOnly | 81.48% | +0.06pp | 40,126 | 10 us | 18 us |
| kFlushAndCompaction | 81.34% | -0.08pp | 40,081 | 9 us | 17 us |
| Leaper (prefetch only) | 81.42% | +0.00pp | 40,085 | 12 us | **60 us** |

Everything sits inside +/-0.08pp, **including RocksDB's own built-in warming**.
The reason is in the training log: at the same write rate that gives LevelDB
about 250 compactions in 300 s, RocksDB completed **5 compaction windows**.
There is almost no cache invalidation to fix, so nothing that fixes it moves the
number — not Leaper, and not the feature RocksDB ships for exactly this purpose.

Two things follow.

**The cache invalidation problem is much milder on RocksDB than on LevelDB at
the same workload.** RocksDB's baseline hit ratio is 81.4% against LevelDB's
72.7%, and it reaches that while doing a fiftieth of the compaction work. Any
claim about Leaper's value on RocksDB has to start by establishing that the
problem is present, which at these settings it is not.

**Leaper's prefetch has a visible cost and no visible benefit here.** Read p99
goes from 19 us to 60 us because warming a range means an iterator seek and scan
competing with client reads. On RocksDB the range-granularity warming that the
missing block metadata forces is more expensive than the block-granularity
insert the LevelDB adapter can do, and with nothing to gain that cost is the
whole story.

This is a negative result about the *setting*, not a refutation of the paper.
Establishing whether the prefetcher pays on RocksDB requires a regime where the
problem exists: far heavier write pressure, and storage where a miss is
expensive. The harness supports both (`--write_rate`, and the LevelDB harness's
`--read_delay_us`); running them is the obvious next step rather than a
conclusion that can be asserted now.
