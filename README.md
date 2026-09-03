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
* **It does not reproduce outside that band, and the band is set by reads
  per hot lifetime against cache size — not by the device.** With a cache
  smaller than the working set no prefetcher helps and warming everything
  costs 3-6 points; with a cache larger than it, warming everything beats
  selection (+7.0 vs +2.65pp), because recall then matters more than
  precision. On the slow-storage configuration where Leaper was neutral,
  making hot ranges stay hot five times longer — nothing else changed — takes
  it to +6.9pp over the floor and +4.5pp over warming everything.
* **On slow storage the cheapest policy wins:** warm flush outputs only
  (RocksDB's `kFlushOnly`), +1.3pp for 8k warmed blocks and no model. This is
  the policy the fifth defect had silently reduced "WarmAll" to, which is why
  the pre-M8 documents found it so strong.
* **At the paper's scale on RocksDB — 10 GB of data, a 3 GB block cache, a
  stable hot set — the reproduction succeeds there too, at a tenth of the
  size.** Every warming policy gains over a point of hit ratio (the first
  RocksDB configuration in which any does), Leaper is the best of them at
  +1.55pp over stock, and its margin over warming flush outputs alone is
  +0.16pp against a 0.01pp noise floor. Getting there took three attempts,
  and the two failures are findings: at the paper's *write rate* RocksDB ran
  zero compactions in 200 s, and with a hot set that turns over every 8 s no
  policy helps at all.
* **Run-to-run noise is ~0.3pp** on the slow-storage tables (same seed,
  same binary); differences under that are reported as none.
* **On the paper's own two workload shapes, the answer depends on which one.**
  Reproduced as stationary power laws at the paper's scale, the
  instant-messaging shape (zipf 0.9, write-heavy) is where selection wins by
  the largest margin seen on RocksDB, +2.2pp over warming everything, when
  the table is larger than the cache; at the table's own 8m-row size, where
  it fits in the cache, warming everything wins by 3.8pp instead while
  Leaper keeps the lowest tail latency. The e-commerce shape (zipf 0.3,
  read-heavy) leaves nothing to select: every key range is read every
  second, the model predicts all of them hot, and Leaper's hit ratio equals
  warming everything to four decimals. See H18.
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
around "cache ≈ working set" (+2.9pp over the shared floor on NVMe/128 MB,
+6.9pp on slow storage once hot ranges live 40 s instead of 8); below it
nothing pays; above it recall beats precision and warming everything wins. Every earlier finding in this repository that "WarmAll
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

**The size of the cache-invalidation problem is the engine's write
amplification, and the two engines differ by an order of magnitude on the
same workload.** Under identical writes for 300 s, LevelDB ran 261
compactions and rewrote 9.6 GB (write amplification ~70: a 10 MB L1 under
whole-key-space writes moves one 4 MB file at a time into the 40 MB of L2 it
overlaps); RocksDB ran ~50 and rewrote 0.45-0.67 GB (~3-5), with or without
dynamic level sizing. A prefetcher can only recover what compaction
destroys, so the same model with the same block-level warming is +2.9pp on
LevelDB and +0.14pp on RocksDB at 480 MB — and +1.55pp on RocksDB once the
database is 10 GB and the writes are heavy enough to make it compact. Two earlier explanations for the RocksDB
null — the iterator-based warming, and a race between concurrent jobs in the
adapter — were real defects and were fixed, and neither was the cause.

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

Three tests are load-bearing rather than decorative:

* `gbdt_check` verifies the hand-written LightGBM text-model scorer against
  LightGBM's own predictions (measured: mean |diff| 3.7e-9, max 3.0e-8 over 2000
  rows and 127 trees). A subtly wrong scorer would produce plausible online
  numbers that mean nothing.
* `mapper_check` pins the key-to-range mapping, including the case where
  LevelDB's `FindShortSuccessor` turns a 16-digit key into the single character
  `"1"` — which, restored to full width, is range id 25 billion and used to
  allocate until the process was killed.
* `sst_warm_check` (RocksDB) proves that an `SstFileReader` sharing the DB's
  table factory inserts blocks under the same cache keys the DB's own reader
  looks up — warm a range through the reader, `Get` it through the DB with
  `PerfContext` on, require zero block reads. This is what lets the RocksDB
  adapter warm at block granularity without a patch (`--warm_mode=sst`).

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
* **The RocksDB port re-reads what it warms.** RocksDB's own
  `kFlushAndCompaction` inserts each block into the cache from memory as the
  table builder produces it; a plug-in has to reopen the finished file. Every
  RocksDB comparison here is therefore selection-plus-a-re-read against
  no-selection-and-no-read. Putting the selection inside
  `prepopulate_block_cache` needs a patch to `BlockBasedTableBuilder` and is
  the most worthwhile work left.
* **No phase 1 on RocksDB.** Block cache keys derive from a per-file
  `OffsetableCacheKey` inside the table reader, so eviction is not
  implementable as a plug-in. Phase 2 *is*: `--warm_mode=sst` opens the
  job's output files through an `SstFileReader` that shares the DB's table
  factory and warms exactly the output blocks in predicted-hot ranges, no
  patch needed (`sst_warm_check` proves the cache keys match). With that in
  place Leaper is the best of the RocksDB policies every time (+0.13 to
  +0.15pp) and every time the margin is at the noise floor, because RocksDB
  compacts an order of magnitude less than LevelDB on this workload — see
  the finding above and H15-H16.
* **Latency results are from an emulated slow device**, not real spinning disks.

## Licence

BSD-3-Clause, matching LevelDB. The paper in `paper/` is redistributed under its
own CC BY-NC-ND 4.0 licence; see [`paper/README.md`](paper/README.md).
