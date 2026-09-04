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

**Eight defects were found in this harness, two of which invalidated every
hit ratio measured before them.** Compaction-output warming silently failed
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
| cache smaller than the working set | 0 | -3.3 to -5.8pp |
| cache ≈ working set (the paper's regime) | **+2.9pp** | -0.2pp |
| same, with hot ranges living 5x longer | **+6.9pp** | +2.4pp |
| cache larger than the working set | +2.65pp | **+7.0pp** |

Above the band, recall beats precision and warming everything wins. Below it,
nothing helps and warming everything actively costs several points. The device
is not what separates these cases: the same 128 MB cache on emulated slow
storage gives Leaper nothing at an 8 s hot lifetime and +6.9pp at 40 s.

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
(`prepopulate_block_cache`). On emulated slow storage with a small cache, the
cheapest policy wins outright: warming flush outputs alone is +1.3pp for 8,000
warmed blocks and no model, because the blocks a flush writes are the records
just written, which under a write-then-read workload are the hottest there are.

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
limit of the synthetic generator, not evidence against the paper.

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

**The two phases pull in opposite directions on LevelDB.** Prefetch alone is
+0.87pp on slow storage; adding the eviction phase makes it +0.59pp. The
step-1 model's recall is 0.79 at precision 0.97, so a fifth of the ranges that
will be read are predicted cold and their blocks are dropped while the input
files are still serving reads.

## Results

Two headline matrices, both corrected. Full detail, and thirteen more
configurations, in [`docs/M8-review-followup.md`](docs/M8-review-followup.md).

**LevelDB, NVMe, 128 MB cache, 300 s** — the regime where selection pays:

| policy | hit ratio | vs LRU | prefetch precision |
|---|---|---|---|
| LRU (stock) | 83.34% | — | — |
| EagerEvict | 83.68% | +0.33pp | — |
| IncrementalWarmup (the paper's baseline) | 84.07% | +0.72pp | 0.22 |
| WarmAll | 83.48% | +0.14pp | 0.14 |
| **Leaper (prefetch phase)** | **86.57%** | **+3.23pp** | **0.73** |
| Oracle, one interval of foresight | 88.79% | +5.45pp | 0.88 |

**RocksDB, 10 GB of data, 3 GB block cache, 200 s** — the paper's scale, with
a stable hot set:

| policy | hit ratio | vs stock |
|---|---|---|
| stock (`kDisable`) | 90.10% | — |
| `kFlushOnly` | 91.49% | +1.39pp |
| `kFlushAndCompaction` | 91.40% | +1.30pp |
| **Leaper, block-level warming** | **91.65%** | **+1.55pp** |

Reproduced as the paper's two real workload *shapes* (stationary power laws,
Table 2 of the paper), the answer depends on which one and on how big the
table is relative to the cache. The instant-messaging shape over a table
larger than the cache is where selection wins by the largest margin measured
on RocksDB, +8.71pp against warming everything's +6.47pp. At that table's own
8m-row size, where it fits in the cache, warming everything wins instead,
+19.23pp against +15.48pp, though Leaper holds the lowest tail latency of the
four policies (p99 31 us against stock's 57 and warm-everything's 106). The
read-heavy e-commerce shape leaves nothing to select at all.

## Layout

```
leaper/            engine-independent core (~1,300 lines): collector,
                   LightGBM scorer, multi-step prediction, Algorithm 3,
                   the two-phase policy and six baseline policies
adapters/leveldb/  LevelDB integration plus a 299-line patch (9 files)
adapters/rocksdb/  RocksDB integration with no patch to RocksDB, and no
                   change to leaper/ either, which is the test that the
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

Three tests are load-bearing rather than decorative:

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
| M8 | Review follow-up: eight defects, the paper's own metrics, real traces, and the corrected measurements in section H | [`docs/M8-review-followup.md`](docs/M8-review-followup.md) |

## Known gaps

* **No real database trace yet.** The Twitter cache traces turn out to be the
  wrong instrument for a key-range predictor: anonymised hash-like keys carry
  no locality in byte order, and the 1M-request samples span minutes rather
  than days. Meta's FAST'20 RocksDB traces are the right next dataset, and the
  converter and pipeline are in place (`tools/convert_twitter_trace.py`).
* **The RocksDB port re-reads what it warms.** RocksDB's own
  `kFlushAndCompaction` inserts each block into the cache from memory as the
  table builder produces it; a plug-in has to reopen the finished file. Every
  RocksDB comparison here is therefore selection-plus-a-re-read against
  no-selection-and-no-read. Putting the selection inside
  `prepopulate_block_cache` needs a patch to `BlockBasedTableBuilder`, and it
  is the single most worthwhile piece of work left.
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
