# Leaper — an open reimplementation

A clean-room reimplementation of **Leaper: A Learned Prefetcher for Cache
Invalidation in LSM-tree based Storage Engines**
([PVLDB 13(11):1976-1989](https://doi.org/10.14778/3407790.3407803), included in
[`paper/`](paper/)), first on LevelDB and then on RocksDB.

The original implementation lived inside X-Engine at Alibaba and was never
released. Everything here is written from the published paper alone — no
internal code was used or consulted — so that the artifact can be published and
built on.

---

## ⚠️ Read this before citing any number in this repository

**This is not a replication of the paper's experiments, and its results do not
measure what the paper measured.** Three things differ, and each of them alone
is enough to change the outcome:

| | the paper | here |
|---|---|---|
| **Storage engine** | X-Engine — row cache *and* block cache, extent-based storage with data reuse, long multi-threaded compactions | LevelDB 1.23 and RocksDB 11.8 — block cache only, short compactions, different level geometry |
| **Data** | 10 GB, real production tables | synthetic key-value data, 0.5-2.3 GB |
| **Workload** | real Tmall e-commerce and DingTalk instant-messaging traffic | a synthetic generator written for this repository |
| **Hardware** | spinning disks, where a cache miss costs milliseconds | NVMe, where a miss costs ~20 us; slow storage is *emulated* with a fixed per-read delay |

A learned prefetcher's value is decided by what a wasted cache insertion costs,
and that quantity depends on every row of that table. **Treat the measurements
here as evidence about this setup, not as a verdict on the paper.**

What can fairly be said:

* **Against the paper's own baselines, the reproduction succeeds.** Leaper beats
  Incremental Warmup by 6.3 percentage points of block cache hit ratio here, in
  the same direction and of a similar magnitude to what the paper reports.
* **Against a baseline the paper does not consider — warming every block a
  compaction writes — it does not**, in any of the regimes tested. That baseline
  is trivial to implement, and RocksDB now ships it
  (`prepopulate_block_cache=kFlushAndCompaction`).
* Whether that reverses on real traces and real hardware is **untested**, and is
  the first thing anyone continuing this work should do.

---

## What the paper does

LSM-tree background operations (flush, compaction) rewrite record blocks, which
invalidates the corresponding block cache entries and produces sudden hit-ratio
drops and tail-latency spikes. Frequency-based replacement policies cannot see
this coming, because the statistics they track are attached to the very blocks
compaction destroys. Leaper predicts which *key ranges* will be accessed next —
key ranges are independent of the storage layout, so the prediction survives
compaction — intersects the predicted-hot ranges with block boundaries, and
prefetches the matching blocks as compaction writes them.

## Findings

Full detail in [`docs/`](docs/). The ones that generalise beyond this setup:

**LevelDB's block cache is dead code on 64-bit POSIX.** It mmaps the first 1000
SSTs, and `ReadBlock` marks mmap-backed blocks non-cachable to avoid
double-caching, so `Table::BlockReader` never inserts. Measured on stock 1.23:
2.07M block cache lookups, **0 hits, 0 inserts**. Any block-cache study on
LevelDB has to take it off the mmap path first.

**LevelDB never reclaims the block cache entries of SSTs compaction deleted.**
They sit under a `cache_id` that `Table::Open` will never issue again —
unreachable garbage until LRU happens to evict them. Reclaiming them is worth
+1.9pp of hit ratio on its own, with no model involved. It is a separate
baseline here (`EagerEvict`) precisely so that this free win is not credited to
the learned prefetcher.

**A learned prefetcher's real competitor is "warm everything", not LRU.** Six
hypotheses for why selection should pay were tested and refuted: cache too rich
(swept 27% → 1% of data), misses too cheap (emulated 200 us device), hot set too
dense (100 → 1000 ranges), reads and writes sharing a hot set (correlation
1.0 → 0.2), prediction horizon too short, and insufficient compaction churn. The
mechanism that survives all of them: **`WarmAll` is a recency-of-write prior and
LRU is a recency-of-read prior**, and on an LSM-tree the block a compaction just
wrote is the better bet — while what it displaces, at LRU's cold end, is the
one-hit tail. Warming junk evicts junk, so precision buys nothing.

**The two phases pull in opposite directions on LevelDB.** Prefetch alone is
+1.98pp; adding the eviction phase makes it -0.72pp. The step-1 model's recall
is 0.807, so a fifth of the ranges that will be read are predicted cold and
their blocks are dropped while the input files are still serving reads.

## Layout

```
leaper/            engine-independent core: collector, LightGBM scorer,
                   multi-step prediction, Algorithm 3, two-phase policy,
                   and the six baseline policies
adapters/leveldb/  LevelDB integration + a 277-line patch (8 files)
adapters/rocksdb/  RocksDB integration — no patch to RocksDB, and no change
                   to leaper/ either, which is the test that the split is real
bench/             workload driver and measurement instrumentation
tools/             offline training, phase calibration, oracle generation,
                   plotting, result summarisation
docs/              methodology and results, one file per milestone
experiments/       reproducible run scripts
paper/             the paper this reimplements
third_party/       vendored LevelDB (1.23) and RocksDB (11.8) as submodules
```

## Build

```sh
./scripts/setup.sh                       # submodules + apply the LevelDB patch
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/leaper/mapper_check              # key -> range mapping regression test
```

`scripts/setup.sh` prints the extra steps for the RocksDB half, which needs a
full RocksDB build.

Two tests are load-bearing rather than decorative:

* `gbdt_check` verifies the hand-written LightGBM text-model scorer against
  LightGBM's own predictions (measured: mean |diff| 3.7e-9, max 3.0e-8 over 2000
  rows and 127 trees). A subtly wrong scorer would produce plausible online
  numbers that mean nothing.
* `mapper_check` pins the key-to-range mapping, including the case where
  LevelDB's `FindShortSuccessor` turns a 16-digit key into the single character
  `"1"` — which, restored to full width, is range id 25 billion and used to
  allocate until the process was killed.

## Reproducing

```sh
./experiments/run_m0.sh    # does the phenomenon exist on stock LevelDB?
./experiments/run_m4.sh    # the baseline matrix on LevelDB
./experiments/run_m7.sh    # the baseline matrix on RocksDB
```

Each script runs the whole protocol: train on one seed, calibrate the two-phase
constants from that run's own compaction log, then evaluate every policy on a
*different* seed from an identical database. Environment variables select the
regime (`CACHE_MB`, `READ_DELAY_US`, `OP_RATE`, `WRITE_CORR`, `RANGE_SIZE`, ...).

## Milestones

| | scope | notes |
|---|---|---|
| M0 | Measurement harness; reproduce the phenomenon on stock LevelDB | [`docs/M0-methodology.md`](docs/M0-methodology.md) |
| M1 | Trace collection, key range selection, features, offline model | [`docs/M1-findings.md`](docs/M1-findings.md) |
| M2-M3 | Online collector, inference, two-phase prefetcher on LevelDB | [`docs/M2-M3-leveldb-integration.md`](docs/M2-M3-leveldb-integration.md) |
| M4 | Baseline matrix, oracle upper bound, regime sweeps | [`docs/M4-results.md`](docs/M4-results.md) |
| M5-M7 | Core/adapter split and the RocksDB port | [`docs/M5-M7-rocksdb.md`](docs/M5-M7-rocksdb.md) |

## Known gaps

* **No real traces.** Every negative result here traces back to a property of the
  synthetic workload. Driving the pipeline with Meta's FAST'20 RocksDB traces or
  Twitter's twemcache traces is the highest-value next step and would settle
  whether the limitation is the workload or the method.
* **Algorithm 1 does not terminate on these workloads.** The paper's efficient-
  expansion criterion keeps saying "expand" once the access matrix reaches a
  stable occupancy, all the way to three key ranges for the whole database. A
  floor on the range count is added here, and the reported range size is
  determined by that floor rather than by the paper's criterion.
* **No phase 1 on RocksDB.** Block cache keys derive from a per-file
  `OffsetableCacheKey` inside the table reader, so eviction is not implementable
  as a plug-in there.
* **Latency results are from an emulated slow device**, not real spinning disks.

## Licence

BSD-3-Clause, matching LevelDB. The paper in `paper/` is redistributed under its
own CC BY-NC-ND 4.0 licence; see [`paper/README.md`](paper/README.md).
