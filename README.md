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

A further warning specific to this repository: **two measurement defects
were found and fixed during the review follow-up (M8), and every hit ratio
measured before that point is wrong.** Compaction-output warming silently
failed (only flush outputs were ever warmed), and the harness counted
LevelDB's own compaction reads as workload lookups — a third of all lookups,
and a share that the warming policies themselves changed. The headline
conclusion flipped twice as these were fixed. The corrected measurements are
section H of [`docs/M8-review-followup.md`](docs/M8-review-followup.md); the
earlier documents are kept with banners saying what is superseded.

What can fairly be said, on the corrected measurements:

* **The paper's result reproduces in the paper's regime — a cache about the
  size of the working set.** On NVMe with a 128 MB cache, Leaper's prefetch
  is +3.2pp of block cache hit ratio over stock LRU and +2.9pp over the floor
  every warming policy shares (reclaiming dead blocks), with 73% of its
  prefetched blocks read; warming every block a compaction writes is neutral
  and adds latency spikes; an oracle shows two more points of headroom.
* **It does not reproduce outside that band.** With a cache smaller than the
  working set no prefetcher helps and warming everything costs 3-6 points;
  with a cache larger than it, warming everything beats selection (+7.0 vs
  +2.65pp), because recall then matters more than precision. Whether a
  deployment sits in the band is decided by cache size, operation rate and how
  long a hot range stays hot, not by the device.
* **On slow storage the cheapest policy wins:** warm flush outputs only
  (RocksDB's `kFlushOnly`), +1.3pp for 8k warmed blocks and no model. This is
  the policy the fifth defect had silently reduced "WarmAll" to, which is why
  the pre-M8 documents found it so strong.
* **Run-to-run noise is ~0.3pp** on the slow-storage tables (same seed,
  same binary); differences under that are reported as none.
* Whether any of this transfers to real traces and real hardware is
  **untested**, and remains the first thing anyone continuing this work should
  do.

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

**A learned prefetcher's real competitors are "warm everything" and "warm
flush outputs only", not LRU** — and which of the three wins is a property of
the cache-to-working-set ratio, not of the method. Selection pays in a band
around "cache ≈ working set" (+2.9pp over the shared floor on NVMe/128 MB);
below it nothing pays; above it recall beats precision and warming
everything wins. Every earlier finding in this repository that "WarmAll
dominates" was measured with compaction-output warming broken and
compaction reads in the denominator, and does not survive the fix.

**Measure what the workload sees, not what the cache sees.** LevelDB's
compaction thread looks up every input block in the block cache
(`fill_cache=false` only stops the insert), and a policy that warms from that
thread slows compaction and so removes a third of those near-certain misses
from its own denominator. This bias was worth +2pp to WarmAll and inverted
the oracle-lookahead result. The harness now counts the workload's threads
only, on both engines.

**Where the warm read runs is a first-order cost on slow storage.** Warming
every output block from the compaction thread, at 200 us a block, halves
compaction throughput (59 → 36 compactions in 180 s). The repository charges
that cost to the compaction thread by default and, optionally, to a
separate thread (`--warm_async`); the two bracket a real device.

**The two phases pull in opposite directions on LevelDB.** Prefetch alone is
+0.87pp on slow storage; adding the eviction phase makes it +0.59pp. The
step-1 model's recall is 0.807, so a fifth of the ranges that will be read
are predicted cold and their blocks are dropped while the input files are
still serving reads.

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
*different* seed from an identical database. `tools/paper_metrics.py` then
reports a matrix in the paper's own terms — miss rate inside "during and after
compaction" windows, latency spikes, overhead from unthrottled runs, `|C ∩ M_i|`
and prefetch precision — rather than as a whole-run hit ratio. Environment variables select the
regime (`CACHE_MB`, `READ_DELAY_US`, `OP_RATE`, `WRITE_CORR`, `RANGE_SIZE`, ...).

## Milestones

| | scope | notes |
|---|---|---|
| M0 | Measurement harness; reproduce the phenomenon on stock LevelDB | [`docs/M0-methodology.md`](docs/M0-methodology.md) |
| M1 | Trace collection, key range selection, features, offline model | [`docs/M1-findings.md`](docs/M1-findings.md) |
| M2-M3 | Online collector, inference, two-phase prefetcher on LevelDB | [`docs/M2-M3-leveldb-integration.md`](docs/M2-M3-leveldb-integration.md) |
| M4 | Baseline matrix, oracle upper bound, regime sweeps | [`docs/M4-results.md`](docs/M4-results.md) |
| M5-M7 | Core/adapter split and the RocksDB port | [`docs/M5-M7-rocksdb.md`](docs/M5-M7-rocksdb.md) |
| M8 | Review follow-up: six defects fixed, the paper's own metrics, real traces, and the corrected re-measurement (section H) | [`docs/M8-review-followup.md`](docs/M8-review-followup.md) |

## Known gaps

* **Real traces: only Twitter cache-trace samples so far**, and those turn out to
  be the wrong instrument for a key-range predictor (anonymised hash-like keys
  carry no locality in byte order; 1M-request samples span minutes, not days).
  A *database* trace with structured keys — Meta's FAST'20 RocksDB traces — is
  what would settle whether the limitation is the workload or the method. The
  converter and pipeline are in place (`tools/convert_twitter_trace.py`).
* **Algorithm 1 does not terminate on the synthetic workloads** (it does on the
  real Twitter samples). A floor on the range count is added for the synthetic
  case, and there the reported range size is determined by that floor rather
  than by the paper's criterion.
* **No phase 1 on RocksDB, and phase 2 is structurally weaker there.** Block
  cache keys derive from a per-file `OffsetableCacheKey` inside the table
  reader, so eviction is not implementable as a plug-in, and the zero-patch
  adapter can only warm by scanning predicted-hot ranges through a DB
  iterator rather than by touching the compaction's output blocks. Its
  result on RocksDB is a null within noise; see H12-H13 for what a longer
  scan does.
* **Latency results are from an emulated slow device**, not real spinning disks.

## Licence

BSD-3-Clause, matching LevelDB. The paper in `paper/` is redistributed under its
own CC BY-NC-ND 4.0 licence; see [`paper/README.md`](paper/README.md).
