# Leaper — an open reimplementation

A clean-room reimplementation of **Leaper: A Learned Prefetcher for Cache
Invalidation in LSM-tree based Storage Engines**
([PVLDB 13(11):1976-1989](https://doi.org/10.14778/3407790.3407803), included in
[`paper/`](paper/)), on LevelDB and on RocksDB.

The original implementation lived inside X-Engine at Alibaba and was never
released. Everything here is written from the published paper alone. No
internal code was used or consulted, so the artifact can be published and
built on.

---

## Read this before citing any number

**This is not a replication of the paper's experiments.** Three things differ,
and each alone is enough to change the outcome:

| | the paper | here |
|---|---|---|
| **Engine** | X-Engine: row cache *and* block cache, extent-based storage with data reuse, long multi-threaded compactions | LevelDB 1.23 and RocksDB 11.8: block cache only, different level geometry, an order of magnitude more or less compaction depending on which |
| **Data** | 10 GB, real production tables | synthetic key-value data, 0.5 GB to 10 GB |
| **Workload** | real Tmall e-commerce and DingTalk instant-messaging traffic | a synthetic generator written for this repository |
| **Hardware** | spinning disks, where a miss costs milliseconds | NVMe, where a miss costs ~20 us; slow storage is *emulated* with a fixed per-read delay |

A learned prefetcher's value is decided by what a wasted cache insertion
costs, and that quantity depends on every row of that table. **Treat the
measurements here as evidence about this setup, not as a verdict on the
paper.**

**Seventeen defects were found in this harness, two of which invalidated every
hit ratio measured before them; eight came from an independent code review
on 2026-09-19 (`docs/code-review-2026-09-19.md`), the seventeenth from
chasing a result that looked too good (M9: a monitor-clock drift that
inflated one QPS column, not a hit ratio), and the numbers below were
re-measured with them fixed.** Compaction-output warming silently failed
for months of work because a hook fired before the output file was synced, and
the hit ratio counted the engine's own compaction reads as workload lookups,
which handed a free 2pp to whichever policy slowed compaction down the most.
The headline conclusion flipped twice as these were fixed. Only section H of
[`docs/M8-review-followup.md`](docs/M8-review-followup.md) is current; the
earlier documents are kept, with banners, as the record of how the conclusions
moved.

---

## What the paper does

LSM-tree background operations rewrite record blocks, which invalidates the
matching block cache entries and produces sudden hit-ratio drops and
tail-latency spikes. Frequency-based replacement cannot see this coming,
because the statistics it keeps are attached to the very blocks compaction
destroys. Leaper predicts which *key ranges* will be read next, since key
ranges survive the storage layout changing, intersects the predicted-hot
ranges with the block boundaries of what compaction is writing, and prefetches
the matching blocks as they are produced.

## What reproduces

**The offline model reproduces cleanly.** Eighteen features (six read rates,
six write rates, three timestamp fields, three precursor rates), LightGBM,
multi-step prediction, and Algorithm 2's precursor discovery, all rebuilt from
the paper. On the workloads where the prediction target carries information,
the model reaches precision 0.98 to 0.999 at recall 0.77 to 0.96, beating the
naive rule "hot in the last interval means hot in the next" by the margin the
paper reports.

**The online result reproduces inside a band, and the band is narrow.** What
decides it is how many reads a hot key range receives during its lifetime,
measured against the cache size. Every warming policy except stock also
reclaims the block cache entries of deleted SSTs, so the value of *warming* is
a policy's distance from that floor (`EagerEvict`), not from LRU:

| regime | Leaper vs floor | warm everything vs floor |
|---|---|---|
| cache smaller than the working set | 0 | -2.9 to -5.8pp |
| cache ≈ working set (the paper's regime) | **+3.0pp** | -0.2pp |
| same, with hot ranges living 5x longer | **+6.6pp** | +1.9pp |
| cache larger than the working set | +2.65pp | **+7.0pp** |

Above the band, recall beats precision and warming everything wins. Below it,
nothing helps and warming everything actively costs several points. The device
is not what separates these cases: the same 128 MB cache on emulated slow
storage gives Leaper nothing at an 8 s hot lifetime and +6.6pp at 40 s.

Run-to-run noise, same seed and binary, is 0.3pp on the slow-storage tables,
0.2pp on NVMe and 0.01pp on the RocksDB paper-scale configuration. Differences
smaller than that are reported here as none.

## Findings that generalise beyond this setup

**LevelDB's block cache is dead code on 64-bit POSIX.** It mmaps the first
1000 SSTs, and `ReadBlock` marks mmap-backed blocks non-cachable to avoid
double caching, so `Table::BlockReader` never inserts. Measured on stock 1.23:
2.07M block cache lookups, **0 hits, 0 inserts**. Any block-cache study on
LevelDB has to take it off the mmap path first.

**LevelDB never reclaims the block cache entries of SSTs compaction deleted.**
They sit under a `cache_id` that `Table::Open` will never issue again,
unreachable garbage until LRU happens to evict them. Reclaiming them is worth
+0.3 to +3.6pp of hit ratio with no model involved, which is why it is a
separate baseline here rather than part of Leaper's score.

**A learned prefetcher's real competitors are "warm everything" and "warm
flush outputs only", not LRU.** Both are trivial, and RocksDB ships them
(`prepopulate_block_cache`). On a cache smaller than the working set no
warming policy beats reclaiming dead blocks: the ones that warm little
(flush outputs only, Leaper, a one-interval oracle) sit on that floor within
noise and the ones that warm a lot lose 2-3 points. Warming everything also
makes the cache a function of how much compaction the run happened to do:
on the ZippyDB model its outcome spans 65% to 78% across seeds, in exact
order of the compaction volume, while Leaper's stays within 0.2pp. An earlier version of
this file had flush-only warming winning that regime by half a point; that
was the ninth defect, a nested flush leaving its flag set so the policy also
warmed part of each compaction, and it is gone.

**The size of the cache-invalidation problem is the engine's write
amplification, and two engines differ by an order of magnitude on the same
workload.** Under identical writes for 300 s, LevelDB ran 261 compactions and
rewrote 9.6 GB, a write amplification near 70, because a 10 MB L1 under
whole-key-space writes moves one 4 MB file at a time into the 40 MB of L2 it
overlaps. RocksDB ran about 50 and rewrote 0.45 to 0.67 GB, near 5, with or
without dynamic level sizing. A prefetcher can only recover what compaction
destroys. On a 10 GB database at the paper's *write rate*, RocksDB performed
**zero compactions in 200 seconds**.

**Selection needs cold ranges to exist.** On a stationary power law at 24,000
to 51,000 reads per second, nearly every key range is touched every second, so
"will this range be read next interval" is true almost everywhere and there is
nothing for a model to select. In the most extreme configuration measured, the
positive rate was exactly 1.000, the model predicted every range hot, and
Leaper's hit ratio equalled warming everything to four decimal places. The
paper's models reach 0.95 precision and recall on real Tmall and DingTalk
traces, so on that data the label clearly does carry information. This is a
limit of the synthetic generator, not evidence against the paper: on
Facebook's FAST'20 ZippyDB model (M9), 29% of the 2,000-key ranges are hot
in a given second and there is something to select.

**The value of learned selection is write amplification times cache
pressure, and the same workload model puts the two engines at opposite ends
of that curve.** On the ZippyDB model, RocksDB rewrites about six times what
it ingests and every policy that warms compaction output is worth +0.8 to
+0.9pp, Leaper included: there is nothing to recover, so warming everything
is free. LevelDB rewrites 700 times its ingest into a 256 MB cache that
holds the hot range with room to spare; there warming everything loses
8pp by evicting the working set, and Leaper, warming the 29% that will be
read, gains +11.3pp — the largest margin over any heuristic measured
here, and the paper's regime.

**A policy's cost on the background thread is a confound, and the control
for it is a dry run.** Leaper's inference on LevelDB's single background
thread (25,000 candidate ranges per job, 5 us each) took 72% of the run and
cut compactions from 1,783 to 671; the first reading of the +11pp above was
that fewer compactions meant fewer invalidations and the gain was an
artefact. `--leaper_dry_run=1` pays the same inference and discards every
prediction: +0.97pp, of which +0.70 is the dead-block floor. The throttling
was worth a third of a point; the prefetching was the rest. The cost is
real, though, so predictions are now memoised within a second
(`Options::memoize_predictions`: the ten jobs LevelDB starts in a second
were making the same 25,000 predictions ten times over), and the benches
warn when inference exceeds 20% of a run.

**Measure what the workload sees, not what the cache sees.** LevelDB's
compaction thread looks up every input block in the block cache
(`fill_cache=false` only suppresses the insert), and a policy that warms from
that thread slows compaction down and so removes a third of those
near-certain misses from its own denominator. That bias was worth +2pp to
"warm everything" and inverted the oracle-lookahead result. The harness now
counts only its own worker threads, on both engines.

**Where the warm read runs is a first-order cost on slow storage.** Warming
every output block from the compaction thread, at 200 us a block, halves
compaction throughput: 59 compactions become 36 in the same 180 s. The
harness can charge that cost to the compaction thread (the default) or to a
separate thread (`--warm_async`), and the two bracket what a real device would
do. The verdict does not change between them.

**Phase 1, the eviction phase, adds nothing measurable on LevelDB, at any
compaction length reached.** With and without it Leaper is within 0.1pp on
both devices at 2 s compactions, and a sweep of the compaction length to
6 s and 28 s at the same compaction volume (M9, section 5) finds the same:
the eviction phase alone sits on the dead-block floor, and on top of
prefetching it is -0.25, +0.29 and -0.14pp, inside the noise. The space it
frees is not scarce: any policy that reclaims dead blocks already runs the
cache 10-20% empty on average, and a big compaction leaves it nearly empty
at the moment it ends. Leaper here is a prefetcher; the eviction phase
stays in the code as an option for engines whose compactions are long,
parallel and small relative to the cache. An earlier version of this file
had the two phases pulling in opposite directions; that was measured with
the ninth and eleventh defects present.

## Results

Three headline matrices, all corrected. Full detail, and thirteen more
configurations, in [`docs/M8-review-followup.md`](docs/M8-review-followup.md)
and [`docs/M9-journal-prep.md`](docs/M9-journal-prep.md).

**LevelDB, NVMe, 128 MB cache, 300 s** — the regime where selection pays:

| policy | hit ratio | vs LRU | prefetch precision |
|---|---|---|---|
| LRU (stock) | 83.54% | — | — |
| EagerEvict | 83.81% | +0.27pp | — |
| IncrementalWarmup (the paper's baseline) | 84.74% | +1.21pp | 0.21 |
| WarmAll | 83.58% | +0.04pp | 0.12 |
| WarmFlushOnly | 83.75% | +0.21pp | 0.09 |
| **Leaper (prefetch phase)** | **86.79%** | **+3.25pp** | **0.72** |
| Oracle, one interval of foresight | 88.79% | +5.26pp | 0.88 |

Over four more evaluation seeds (M9, section 4): LRU 83.30 ± 0.33%,
EagerEvict +0.19 ± 0.03pp, WarmAll -0.02 ± 0.13pp (mixed sign), Leaper
**+2.99 ± 0.12pp** (+2.84 to +3.11), the same sign on every seed.

**RocksDB, 10 GB of data, 3 GB block cache, 200 s** — the paper's scale, with
a stable hot set:

| policy | hit ratio | vs stock |
|---|---|---|
| stock (`kDisable`) | 90.11% | — |
| `kFlushOnly` | 91.51% | +1.40pp |
| `kFlushAndCompaction` | 91.40% | +1.29pp |
| **Leaper, block-level warming** | **91.67%** | **+1.56pp** |

Over four more evaluation seeds (M9, section 4), through the patched
prepopulate path: stock 90.53 ± 0.24%, `kFlushOnly` +1.38 ± 0.21pp,
`kFlushAndCompaction` +1.39 ± 0.32pp, Leaper +1.65 ± 0.09pp. Leaper's
paired margin over `kFlushAndCompaction` is +0.26 ± 0.30 (every seed
positive, the smallest +0.02) and over `kFlushOnly` +0.27 ± 0.25 with one
seed negative: **the three warming policies are within a third of a point
of each other at this scale, and their ordering is inside the seed
spread.** Leaper's margin over stock is the most repeatable of the three.

Reproduced as the paper's two real workload *shapes* (stationary power laws,
Table 2 of the paper), the answer depends on which one and on how big the
table is relative to the cache. The instant-messaging shape over a table
larger than the cache is where selection wins by the largest margin measured
on RocksDB, +8.36pp against warming everything's +6.49pp. At that table's own
8m-row size, where it fits in the cache, warming everything wins instead,
+19.11pp against +15.35pp. (Tail latency is not repeatable across runs of
this configuration — the same policy's mean per-second p99 varied threefold
between two identical runs — so no ordering is claimed for it.) The
read-heavy e-commerce shape leaves nothing to select at all.

**LevelDB, Facebook's FAST'20 ZippyDB model (`--key_dist=mixgraph`), 50M
records, 256 MB cache, 300 s** — the paper's regime: the hot range fits in
the cache, and compaction rewrites 700x what the workload writes:

| policy | hit ratio | vs LRU | compactions in window | prefetch precision |
|---|---|---|---|---|
| LRU (stock) | 73.56% | — | 1,783 | — |
| EagerEvict | 74.26% | +0.70pp | 1,739 | — |
| IncrementalWarmup (the paper's baseline) | 72.02% | -1.53pp | 1,628 | 0.13 |
| WarmAll | 65.38% | -8.18pp | 1,606 | 0.08 |
| WarmFlushOnly | 74.44% | +0.89pp | 1,743 | 0.75 |
| Leaper (prefetch phase), inference on the compaction thread | 84.61% | +11.05pp | 671 | 0.66 |
| Leaper, same inference, predictions discarded (dry run) | 74.53% | +0.97pp | 668 | — |
| **Leaper (prefetch phase), predictions memoised** | **84.86%** | **+11.32pp** | 1,066 | 0.57 |

Over four more evaluation seeds (M9, section 4): LRU 73.57 ± 0.01%,
WarmFlushOnly +0.92 ± 0.10pp, Leaper **+11.24 ± 0.08pp** (+11.19 to
+11.35) and +10.32 ± 0.04pp over WarmFlushOnly. WarmAll is -1.10 ± 4.01pp,
from 70.1% to 78.5% — its outcome tracks the compaction volume of the run
exactly (26.7 to 62.2 GB across five seeds), Leaper's does not.

On RocksDB the same model, at 256 MB of cache, gives every policy that warms
compaction output +0.8 to +0.9pp and Leaper equal to `kFlushAndCompaction`
(-0.02 ± 0.01pp over four seeds); the same table, both engines, is in M9.

## Layout

```
leaper/            engine-independent core (~1,300 lines): collector,
                   LightGBM scorer, multi-step prediction, Algorithm 3,
                   the two-phase policy and six baseline policies
adapters/leveldb/  LevelDB integration plus a 299-line patch (9 files)
adapters/rocksdb/  RocksDB integration: a zero-patch mode (warm by
                   re-reading output files; builds against pristine 11.8)
                   and a 43-line RocksDB patch that makes
                   prepopulate_block_cache selective (required for
                   warm_mode=prepop; CMake detects it); no change to
                   leaper/ either way, which is the test that the
                   core/adapter split is real
bench/             workload driver and measurement instrumentation
tools/             offline training, phase calibration, oracle generation,
                   trace conversion, plotting, result summarisation
docs/              methodology and results, one file per milestone
experiments/       reproducible run scripts
paper/             the paper this reimplements
third_party/       vendored LevelDB 1.23 and RocksDB 11.8 as submodules
```

The policies the harness can run, all through the same core:
`off`, `eager_evict`, `incremental_warmup` (the paper's baseline), `warm_all`
(= `kFlushAndCompaction`), `warm_flush` (= `kFlushOnly`), `leaper`, and
`oracle`, an offline upper bound that replays future accesses.

## Build

```sh
./scripts/setup.sh                       # submodules + apply the LevelDB patch
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/leaper/mapper_check              # key -> range mapping regression test
```

`scripts/setup.sh` prints the extra steps for the RocksDB half, which needs a
full RocksDB build.

Five tests are load-bearing rather than decorative:

* **`gbdt_check`** verifies the hand-written LightGBM text-model scorer
  against LightGBM's own predictions: mean |diff| 3.7e-9, max 3.0e-8 over
  2,000 rows and 127 trees. A subtly wrong scorer would produce plausible
  online numbers that mean nothing.
* **`mapper_check`** pins the key-to-range mapping, including the case where
  LevelDB's `FindShortSuccessor` turns a 16-digit key into the single
  character `"1"`, which restored to full width is range id 25 billion and
  allocated until the process was killed.
* **`sst_warm_check`** (RocksDB) proves that an `SstFileReader` sharing the
  DB's table factory inserts blocks under the same cache keys the DB's own
  reader looks up: warm a range through the reader, `Get` it through the DB
  with `PerfContext` on, require zero block reads from the file. This is what
  lets the RocksDB adapter warm at block granularity with no patch
  (`--warm_mode=sst`).
* Two script-level checks from the M9 review:
  `scripts/check_pristine_rocksdb_build.sh` (the adapter and bench compile
  against an unpatched RocksDB 11.8; only `warm_mode=prepop` needs the
  patch) and `scripts/check_run_m4_oracle_binding.sh` (a matrix rerun under
  a new evaluation seed makes its own oracle or refuses a mismatched one).
* **`budget_check`** (RocksDB) pins the per-job warm budget on the case that
  slipped past three revisions of it: a file in which the predicted ranges
  have no keys, where a Seek reads a block without the scan loop ever
  running. 355 blocks without a budget, exactly 32 with one.

## Reproducing

```sh
./experiments/run_m0.sh             # does the phenomenon exist on stock LevelDB?
./experiments/run_m4.sh             # the policy matrix on LevelDB
./experiments/run_m7.sh             # the policy matrix on RocksDB
./experiments/run_m7_paperscale.sh  # RocksDB at the paper's 10 GB / 3 GB scale
./tools/report_m8.sh                # every result in this repository, in one table
```

Each script runs the whole protocol: train on one seed with the policy off,
calibrate the two-phase constants from that run's own compaction log, then
evaluate every policy on a *different* seed from a freshly filled database.
`tools/paper_metrics.py` reports the matrix in the paper's own terms, that is
miss rate inside "during and after compaction" windows, latency spikes,
overhead from unthrottled runs, `|C ∩ M_i|` and prefetch precision, rather
than as a whole-run hit ratio.

Environment variables select the regime, and the ones that matter most are
`CACHE_MB`, `READ_DELAY_US`, `LIFETIME_S`, `NUM_KEYS`, `OP_RATE`,
`WRITE_RATE`, `RANGE_SIZE` and `KEY_DIST`, plus `READ_DELAY_US` on LevelDB
and `LEVEL_BASE_MB`, `L0_TRIGGER` and `DYNAMIC_LEVEL_BYTES` on RocksDB. Which
warming path the RocksDB adapter uses is chosen by the policy name:
`sst_leaper` warms the job's output blocks, `leaper` scans predicted-hot
ranges through a DB iterator.

## Milestones

| | scope | notes |
|---|---|---|
| M0 | Measurement harness; reproduce the phenomenon on stock LevelDB | [`docs/M0-methodology.md`](docs/M0-methodology.md) |
| M1 | Trace collection, key range selection, features, offline model | [`docs/M1-findings.md`](docs/M1-findings.md) |
| M2-M3 | Online collector, inference, two-phase prefetcher on LevelDB | [`docs/M2-M3-leveldb-integration.md`](docs/M2-M3-leveldb-integration.md) |
| M4 | Baseline matrix, oracle upper bound, regime sweeps | [`docs/M4-results.md`](docs/M4-results.md) |
| M5-M7 | Core/adapter split and the RocksDB port | [`docs/M5-M7-rocksdb.md`](docs/M5-M7-rocksdb.md) |
| M8 | Review follow-up: sixteen defects over three review rounds, the paper's own metrics, real traces, and the corrected measurements in section H | [`docs/M8-review-followup.md`](docs/M8-review-followup.md) |
| M9 | Toward the journal version: selective prepopulate on RocksDB (the re-read never mattered), the FAST'20 ZippyDB model on both engines, the dry-run control, the seventeenth defect, memoised predictions, four-seed variance for the six headline cells, the phase-1 sweep (compactions of 2, 6 and 28 s) | [`docs/M9-journal-prep.md`](docs/M9-journal-prep.md) |

## Known gaps

* **No real database trace.** The Twitter cache traces are the wrong
  instrument for a key-range predictor (anonymised hash-like keys carry no
  locality in byte order, and the 1M-request samples span minutes), and
  Meta's FAST'20 RocksDB traces were never released. What exists is the
  *model* Meta fitted to ZippyDB and shipped in `db_bench`, ported here as
  `--key_dist=mixgraph` (M9). It has fixed range hotness and no movement of
  the hot set, so it tests selective warming, not learning: the learned
  model's AUC on it is 0.76 against 0.66 for "hot last interval, hot next".
* **The RocksDB results in section H were measured with the plug-in
  re-reading what it warms**, against a built-in that warms from memory.
  Closed in M9: through the patched prepopulate path Leaper lands within
  0.2pp of the re-read path on all three configurations, so no section-H
  RocksDB margin was a cost artefact. The section-H rows are kept as the
  zero-patch result.
* **The prefetch horizon has to be short against the hot set's
  lifetime.** With 64 MB SSTs a compaction runs 28 s and the prefetch
  phase must predict 33-40 s ahead on a workload whose hot ranges live
  40 s: at a 1 s interval the 24-step horizon clamps (the core counts
  these as clamped predictions), and at a 5 s interval nothing clamps but
  the prefetch precision falls to 0.20 and no policy is outside the noise
  (M9, section 5). The paper's setting, compactions of minutes against
  hot sets of hours, satisfies the condition; this one does not.
* **Four evaluation seeds per headline cell, on one laptop.** The seed
  spread settles every ordering claimed here except one: at the paper's
  scale on RocksDB the three warming policies are within a third of a point
  and their order is inside the spread. The models are trained once (seed
  42); training-set variance is not measured.
* **No phase 1 on RocksDB.** Block cache keys derive from a per-file
  `OffsetableCacheKey` held inside the table reader, so eviction is not
  implementable as a plug-in there. Phase 2 is, through `--warm_mode=sst`.
* **Algorithm 1 does not terminate on the synthetic workloads**, though it
  does on the real Twitter samples. A floor on the range count is applied in
  the synthetic case, so the reported range size comes from that floor rather
  than from the paper's criterion.
* **Latency results come from an emulated slow device**, not real spinning
  disks, and the paper's absolute figures are not testable here in any case,
  since neither the workload nor the hardware matches.

## Licence

BSD-3-Clause, matching LevelDB. The paper in `paper/` is redistributed under
its own CC BY-NC-ND 4.0 licence; see [`paper/README.md`](paper/README.md).
