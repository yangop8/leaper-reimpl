# M8 — Review follow-up: defects, missing pieces, and verifying the paper on its own terms

> **Read section H first.** This document is written in the order the work
> happened, and two measurement defects were found along the way (the fifth
> and sixth, below). Every hit ratio in sections E, F and G — and in the M0-M7
> documents — was measured with one or both of them present and is
> superseded. Section H holds the corrected measurements; the earlier
> sections are kept because the record of how a conclusion flipped twice is
> part of what this artifact has to say.

> **Scope note.** As elsewhere: LevelDB/RocksDB, synthetic workloads and one
> set of real cache-trace samples, NVMe with emulated slow storage. Not a
> replication of the paper's X-Engine measurements.

A code review after M7 found four defects, six pieces of the paper that were
never implemented or never exercised, and — most importantly — that none of the
paper's three headline numbers had been tested on the paper's own terms. This
milestone works through that list in the order the review ranked it.

## Defects found and fixed

| # | defect | effect | fix |
|---|---|---|---|
| 1 | **RocksDB adapter warmed at compaction *begin*** (`leaper_rocksdb.cc`, `Listener::Begin`), before the output files existed | every seek hit the input files and warmed exactly the blocks about to be invalidated; the M7 "Leaper" row (+0.00pp, p99 19→60 us) measured this bug, not the method | predict at Begin, warm at End |
| 2 | **Collector allocated ~460 MB** (`max_ranges` default 2^22, never overridden) | RSS 510 MB vs 48 MB for stock; larger than the block cache in most runs; four orders of magnitude above the paper's 3-16 KB collector; any overhead figure meaningless | sized to the key space; RSS now 57 MB |
| 3 | **Timestamp features skewed by the warmup** between training (trace clock) and inference (run clock) | `minute`/`second` off by 30 in every online prediction | trace meta records the offset; trainer applies it |
| 4 | **Algorithm 3's sort-merge branch was dead code** | only per-block binary search ran; the paper's hybrid cost model never executed | phase 1 now routes through `SelectOverlapping` |

Also moved: phase 1 ran **under the DB mutex** (index walks and thousands of
evictions with every foreground operation blocked). It now runs after
`DoCompactionWork` releases the lock; the inputs are pinned by the compaction's
version reference, so this is safe.

## Missing pieces now implemented

* **Range queries** — `DBIter::Seek` hook (`OnSeek`), scan operations in the
  harness (`--scan_ratio`, `--scan_len`), scan seeks count as reads in the trace.
* **SSAD** (paper 7.4) — the prefetcher switches itself off when the block cache
  miss ratio stays above a threshold for N intervals, and back on when it
  recovers (`--ssad_miss_threshold`, `--ssad_window`). The paper keys it on
  slow-SQL counts; the harness has no SQL, so the miss ratio stands in.
* **Row cache on RocksDB** — `--row_cache_mb`, with `ROW_CACHE_HIT/MISS` reported.
* **The paper's own metrics**, which had never been measured:
  * `|C ∩ M_i|` — blocks resident when a compaction begins (Formulation 2),
    counted by the adapter before the core acts;
  * miss *rate* inside `[op begin, op end + T2]` windows — Table 4's
    "during and after compactions" aggregation, instead of a whole-run mean;
  * latency spikes — seconds whose read p95 exceeds k× the run median;
  * overhead — QPS outside background-operation windows, relative to stock,
    from **unthrottled** runs. Every earlier run was rate-limited, which made
    overhead invisible by construction.
* **Prefetch precision** — the share of prefetched blocks that were read at
  least once before eviction, measured in `StatsCache` at eviction time. This
  is the number behind "warming junk evicts junk".
* **Oracle lookahead** — `make_oracle.py --lookahead W` unions W intervals; W=1
  is the paper's prediction target, larger W approaches a hindsight policy.

## Real traces: Twitter cache-trace samples

Three 1M-request samples from `twitter/cache-trace` (2020Mar), chosen for a
read/write mix near the paper's: cluster019 (get 0.74), cluster007 (get 0.76),
cluster029 (get 0.86). Keys are anonymised strings; each distinct key is
assigned its rank in byte order and stored as a 16-digit decimal, so ranges are
contiguous in the order an LSM would store them (`tools/convert_twitter_trace.py`).

**Algorithm 1 terminates on real data.** On all three samples the paper's
efficient-expansion criterion stops on its own (A* = 300, 190, 60). The
non-termination reported in M1 is a property of the synthetic workload, whose
access matrix reaches a stable occupancy, and **not a gap in the algorithm** —
that earlier claim is withdrawn.

**The naive baseline is near-random on real data.** Last-interval rule AUC is
0.51 / 0.59 / 0.57; births and deaths are 32-44% of rows against 1.4-4.6% on
the synthetic workloads. Real churn is far above what the paper's 0.83-recall
baseline implies, which is consistent with the paper's motivation.

**The model does not beat it at this scale.** On two of three samples LightGBM
collapses to predict-everything-positive (recall 1.0, precision = base rate);
only cluster007 learns anything (AP 0.68 → 0.76, half of births recovered).
Two causes, both established by experiment rather than assumed:

* *Granularity is pinned in the noise regime.* Algorithm 1's α = 0.6 holds the
  access matrix near 40% zeros, which at 1M requests means ~1-2 accesses per
  (range, interval) whatever the interval length: coarser intervals just make
  it pick finer ranges (A* = 120 → 60 → 30 for 5 → 10 → 20 s). The label
  "accessed at least once" is Poisson noise there. Forcing coarse ranges
  (≥ 1000 keys) flips every cell to "accessed" (positive rate 1.0). Between the
  two regimes there is nothing to learn from 500 s of data.
* *Byte order carries no locality for these keys.* The paper's keys are
  auto-increment primary keys, so a key range is a slice of time and of
  application semantics. Twitter's anonymised keys sort by namespace and then
  hash-like; adjacent ranks are unrelated objects, and aggregating them
  averages any structure away.

The conclusion is about the data, not the method: **a cache trace with
hash-like keys is the wrong instrument for a key-range predictor.** The right
public data is a *database* trace with structured keys — Meta's FAST'20 RocksDB
traces (UDB keys derive from MySQL) — and the full Twitter traces (days, not
minutes) would at least resolve the interval-length question. Neither fit in
this session; the converter and pipeline are in place for both.

## Results

### E1 — RocksDB with the timing defect fixed

| policy | hit ratio | vs stock | p95 | p99 |
|---|---|---|---|---|
| kDisable (stock) | 81.43% | — | 11 us | 19 us |
| kFlushOnly | 81.49% | +0.05pp | 10 us | 19 us |
| kFlushAndCompaction | 81.33% | -0.10pp | 13 us | 22 us |
| Leaper (prefetch only) | 81.39% | -0.04pp | 12 us | **20 us** |
| Leaper + 32 MB row cache | 78.81% | -2.63pp | 13 us | 23 us |

The fix removed the cost (p99 back from 60 us to 20 us) without adding a benefit,
which is the honest outcome: at this write rate RocksDB completes ~5 compaction
windows per 300 s and there is nothing to prefetch against. The row-cache row
is not comparable on block cache hit ratio — the row cache absorbs the easy hits
first — and at 32 MB it holds ~300k rows against a 2.5M-key hot set (row cache
hit ratio 4.2%). Repeating M7 under heavy write pressure remains the open step
for RocksDB.

### E2 — the paper's overhead claim, first direct test

Reads unthrottled, writes paced at 4k/s, NVMe, 128 MB cache, 120 s. QPS is
now the metric, and "QPS out" is measured outside background-operation windows
as the paper does.

| policy | QPS (whole run) | QPS out of windows | vs stock | hit ratio |
|---|---|---|---|---|
| stock | 969,055 | 954,502 | — | 98.70% |
| EagerEvict | 873,839 | 861,229 | -9.8% | 98.66% |
| WarmAll | 881,312 | 824,798 | -13.6% | 98.64% |
| Leaper (prefetch only) | 961,808 | 928,773 | **-2.7%** (whole run -0.7%) | 98.77% |
| Leaper, collector sampling P=0.01 | 990,495 | — | +2.2% | 98.81% |

Leaper's cost lands inside the paper's "at most 0.95%" on the whole-run number
and at 2.7% outside windows — but the run-to-run spread at ~1M QPS on this
laptop is at least that wide: EagerEvict is slower than stock by 10% *in seconds
with no compaction at all*, where it does no work, while Leaper — which performs
the same evictions plus collection and inference — is not. A single run cannot
resolve a 1% effect against that noise. Five repeats of stock and Leaper are in
the follow-up chain (F1); until then the overhead claim is **consistent with the
paper but not confirmed**.

The collector at P=0.01 shows no measurable cost, which matches the paper's
Table 1 direction (sampling cut their collector's QPS impact from 17% to 3.7%)
without being precise enough to reproduce the numbers.

### E3 — oracle lookahead

Slow storage, 64 MB cache. The oracle warms a compaction's output blocks whose
range will be read within the next W intervals.

| lookahead W | hit ratio |
|---|---|
| 1 (the paper's prediction target) | 39.15% |
| 5 | 40.99% |
| 20 | **41.32%** |
| WarmAll, for reference | 41.09% |

With 20 intervals of foresight a selective policy finally edges past warming
everything — by 0.23pp. **The value of selection exists, it is small, and it
needs about twenty intervals of lookahead**, against multi-step models whose
recall is 0.25 by step 6. This is the quantitative form of the earlier
observation that the 1-interval target caps what any learned policy can reach.

### E4 — skew sweep (paper Figure 13a)

Slow storage, 64 MB cache, plain zipfian over a contiguous hot region.

| zipf | LRU | WarmAll | Leaper (prefetch only) | Leaper vs LRU |
|---|---|---|---|---|
| 0.0 | 14.01% | 15.10% | 14.93% | +0.92pp |
| 0.3 | 15.52% | 17.23% | 17.02% | +1.50pp |
| 0.5 | 20.29% | 23.27% | 22.81% | +2.52pp |
| 0.9 | 36.39% | 45.77% | 45.18% | **+8.79pp** |
| 1.0 | — | — | — | generator singular (see below) |

The paper's Figure 13(a) says Leaper's gain is absent at zipf 0 and grows with
skew; that shape reproduces. Two things are new. First, **on a stationary
zipfian workload Leaper nearly matches WarmAll** (45.18% vs 45.77% at 0.9):
"hot stays hot" is exactly what the model learns, the hot ranges are
contiguous, and its selection covers what matters. The large gap seen on the
lifecycle workload is a property of that workload's churn, not of the method.
Second, the YCSB zipfian formula is singular at theta = 1 (alpha = 1/(1-theta)),
collapsing every key onto three values that live in the memtable; the block
cache saw zero lookups. The top point is rerun at 0.99 (F2).

### E5 — range queries

10% of reads are 32-record scans; the seek key counts as a read for the
collector and the trainer. Slow storage, 64 MB cache.

| policy | hit ratio | vs LRU | miss rate in op windows | vs stock | prefetch precision |
|---|---|---|---|---|---|
| LRU | 41.89% | — | 58.68% | — | — |
| WarmAll | **48.07%** | +6.19pp | 52.28% | **+10.9%** | 0.181 |
| Leaper (prefetch only) | 43.68% | +1.79pp | 56.86% | +3.1% | (censored; see F4) |
| Oracle (1-interval) | 45.07% | +3.19pp | 55.36% | +5.7% | **0.997** |

Range queries change nothing about the ordering. What the precision column
adds is the sharpest statement yet of why: **the oracle's prefetched blocks are
read 99.7% of the time and WarmAll's 18.1%, and WarmAll wins.** An 82% waste
rate costs nothing when what it displaces is the LRU tail.

### E6 — distribution shift

Model trained on 16 chains with 8 s lifetimes; evaluated on 64 chains with 3 s
lifetimes. Slow storage, 64 MB cache.

| policy | hit ratio | vs LRU |
|---|---|---|
| LRU | 22.96% | — |
| WarmAll | 25.89% | +2.92pp |
| Leaper (prefetch only) | 25.10% | +2.14pp |
| Leaper + SSAD (absolute 0.7) | 24.95% | +1.98pp |

The model is not brittle under this shift: it keeps three quarters of
WarmAll's gain on a workload with four times the chains and lifetimes a third as
long, because what it learned — recent read rate predicts the next interval —
transfers. The SSAD row is not a test of SSAD: an absolute threshold of 0.7 on a
workload whose steady-state miss ratio is 0.75 suspended the prefetcher for
175 of 180 seconds, so that row is EagerEvict with extra steps. SSAD is
re-defined relative to the workload's own trailing miss ratio (30% above its
EWMA) and re-run in F3.

## The fifth defect, found by the new instrumentation

The uncensored prefetch-precision counter (F4) reported Leaper at **0 blocks
inserted** for 21,217 warm calls, WarmAll at 119,003 inserted for 411,790
(29%), and the oracle at 2%. Warming was mostly failing, silently.

The compaction-path `OnOutputFileFinished` hook fired at `db_impl.cc:925`,
**before** `outfile->Sync()` (933) and `Close()` (936). LevelDB's
`PosixWritableFile` buffers 64 KiB; `TableBuilder::Finish` writes the index,
filter and footer into that buffer, so at the moment the hook fired the footer
was not on disk. The warm path opened the file read-only through the table
cache, read a garbage footer, and `FindTable` failed — and
`TableCache::WarmBlock` swallowed the failure (`table_cache.cc:118`). The
flush path calls the hook after `BuildTable`, which syncs and closes
internally, so flush-output warming worked; compaction-output warming never
did. WarmAll's 29% were flush outputs.

**Consequence: every LevelDB result for WarmAll, Leaper and the oracle before
this point measured flush-path prefetch only.** That includes the M4 matrices,
the cache-ratio, correlation and write-heavy sweeps, E3-E6 above, and the
conclusion drawn from them that warming everything dominates selection. The
LRU, EagerEvict and IncrementalWarmup rows stand (they warm nothing).

Fix: the hook now fires after the verification open, once the file is synced,
closed and registered in the table cache with its `cache_id`; `WarmBlock` and
`EvictBlock` return `bool`; the adapter counts failures and the harness prints
a warning when any occur. Regression check on a smoke run: 358,377 warm calls,
**0 failed, 358,377 inserted**. The whole LevelDB matrix is being re-measured
(tags suffixed `_v2`); results follow below.

## G — re-measurement after the fifth defect (superseded by the sixth, kept for the record)

Tags `_v2`. Compaction-path warming now works (0 failed warms in every run).
These numbers still count compaction reads in the denominator — see the next
section — so the hit ratios are biased towards the policies that warm most.
They are kept because the *other* columns stand, and because the ordering
they showed is what exposed the sixth defect.

G1, slow storage (200 us reads), 64 MB cache, 180 s:

| policy | hit ratio | vs LRU | p99 us | blocks warmed | read at least once | compactions in 180 s |
|---|---|---|---|---|---|---|
| LRU | 36.82% | — | 357 | — | — | 57 |
| EagerEvict | 38.50% | +1.68pp | 633 | — | — | — |
| IncrementalWarmup | 36.81% | -0.01pp | 818 | 219,213 | 5.4% | 33 |
| WarmAll | 38.70% | +1.88pp | 362 | 406,015 | 2.8% | 29 |
| Leaper (both phases) | 37.80% | +0.98pp | 356 | 38,972 | 13.2% | — |
| Leaper (prefetch only) | 38.36% | +1.54pp | 575 | 29,262 | 12.8% | 54 |
| Oracle (W=1) | 39.19% | +2.37pp | 839 | 132,454 | 12.3% | 43 |

G2, NVMe, 128 MB cache, 300 s: LRU 72.71%; WarmAll **-1.46pp**; IncrementalWarmup
-2.66pp; EagerEvict +1.82pp; Leaper (prefetch only) **+3.63pp**; Leaper (both)
+1.52pp; Oracle +4.89pp. Prefetch precision: WarmAll 0.14, Leaper 0.72, Oracle
0.89.

G3, oracle lookahead on slow storage: W=1 38.68%, W=5 38.96%, W=20 39.19%.

G4, zipf sweep (LRU / WarmAll / Leaper prefetch-only): 0.0: 13.98 / 12.51 /
12.58; 0.3: 15.41 / 14.12 / 13.80; 0.5: 20.19 / 18.98 / 18.64; 0.9: 37.04 /
39.28 / 38.52; 0.99: 37.98 / 39.05 / 38.32. Prefetching of any kind loses to
LRU below zipf 0.9 on this cache size.

G5, shift at t=120 s (16 chains / 8 s → 64 chains / 3 s), SSAD relative 0.3:
before the shift LRU 32.46%, WarmAll 33.68%, Leaper 33.63%; after it LRU
20.64%, WarmAll 20.30%, Leaper 21.91%. Leaper keeps its edge through the
shift; WarmAll loses its own. SSAD never suspended: the miss ratio rose 18%
relative, under the 30% threshold. Re-run at 10% in the v3 chain.

G6, 10% scans: LRU 40.53%, WarmAll +1.92pp, Leaper (prefetch only) +1.98pp,
Oracle +1.71pp — indistinguishable.

Two things in G1 were already inconsistent with "WarmAll dominates": its
warmed blocks were read 2.8% of the time against Leaper's 12.8%, and it
completed half as many compactions. The second turned out to be the
explanation for the first's not mattering.

## The sixth defect — the hit ratio counted LevelDB's own compaction reads

Found while reading the v2 re-measurement (the one made after the fifth
defect was fixed). The policies did not all issue the same number of block
cache lookups per user read: stock 1.447, Leaper 1.434, oracle 1.340,
IncrementalWarmup 1.252, WarmAll 1.229 — and the reduction tracked how many
blocks each policy warmed. With a 10-bit bloom filter a `Get` should look up
about one data block, so the excess had to come from somewhere other than the
workload.

It came from LevelDB's background thread. Compaction reads every input block
through `Table::BlockReader`, and `fill_cache=false` only suppresses the
insert — the lookup at `table/table.cc:177` happens regardless. The
`StatsCache` decorator counted every lookup that was not marked as a policy
probe, so compaction input reads were in the denominator, and almost all of
them are misses (cold L1+ blocks). On the stock slow-storage run they were
**34% of all lookups** (199,303 of 587,245 in 60 s, 14% hits).

That alone would only depress every policy's hit ratio by a similar amount.
The defect became a bias because **warming is done on the compaction thread**:
each warm is one 200 us emulated read, so WarmAll, warming 406,015 blocks,
doubled the mean compaction time (2.81 s → 5.79 s) and completed 29
compactions in 180 s where stock completed 57. Half the compaction reads
disappeared from WarmAll's denominator — with them a block of near-certain
misses — and its hit ratio rose by roughly 2pp for reasons that have nothing
to do with what the cache held. Every policy that warms a lot (WarmAll,
IncrementalWarmup, oracle) got this credit in proportion to how much it
warmed; Leaper, which warms 15x fewer blocks, got almost none.

The paper's hit ratio is over user requests, and in X-Engine compaction reads
extents directly rather than through the block cache, so the comparable
number is the workload's own. Fix: the harness marks its worker threads, and
`StatsCache` now counts only their lookups; the background thread's lookups
are reported separately (`bg_lookups`, `bg_hits` columns; a line in the
summary). Validation, stock 60 s slow storage:

| counting | lookups | hit ratio |
|---|---|---|
| everything not marked as a policy probe (before) | 587,245 | 34.4% (this run, recomputed; the 180 s v2 matrix had stock at 36.8%) |
| workload threads only (after) | 387,942 | **44.8%** |
| excluded: background thread | 199,303 | 14.2% |

The same 60 s check puts WarmAll at 42.8% by the corrected count, below stock.
That is a single short run and is not the result; the whole matrix is being
re-measured a second time (tags `_v3`), results in section H.

**RocksDB has the same problem in a different form.** The M7 harness read
RocksDB's process-wide `BLOCK_CACHE_DATA_HIT/MISS` tickers, which count
compaction input reads and — worse — the Leaper listener's own warming
iterator, which scans the predicted ranges from the compaction thread and
registers each block it pulls in as a data miss. The RocksDB harness now uses
the thread-local `PerfContext` (data hits = `block_cache_hit_count` less index
and filter hits; data misses = `block_read_count` less index, filter and
dictionary reads) accumulated by the workload threads only, with the ticker
totals kept as the background figure. M7 is re-run as `m7v3`.

**Consequence: every hit-ratio figure in this repository before section H
— M0 through G — was measured with compaction reads in the denominator.**
The direction of the bias is known (it favours whichever policy slows
compaction most), its size is not small (2pp on a 37% baseline), and the M4
regime sweeps, E3-E6 and G1-G6 are all affected. The latency percentiles,
QPS, the prefetch-precision counters and the trace-based offline results
(M1, the Twitter runs) are not: none of them go through the lookup counter.

## F1 — overhead, five repeats: the laptop cannot resolve it

| rep | stock QPS | Leaper QPS | paired |
|---|---|---|---|
| 1 | 846,776 | 1,079,394 | +27.5% |
| 2 | 1,112,527 | 995,688 | -10.5% |
| 3 | 734,501 | 655,555 | -10.8% |
| 4 | 1,090,482 | 1,060,180 | -2.8% |
| 5 | 1,094,642 | 1,078,536 | -1.5% |
| mean | 975,786 ± 173,812 (**17.8%**) | 973,871 ± 181,205 | mean-of-ratios +0.4%, paired sd 15.7% |

Stock alone varies by ±18% between identical runs at ~1M QPS on this machine;
a 0.95% effect is an order of magnitude below the noise floor. End-to-end
throughput cannot test the paper's overhead claim here, and the paper's own
measurement used a dedicated 96-thread server. The figure that *can* be
reproduced is Table 5's per-component form:

| component | paper | here |
|---|---|---|
| collector, per query | < 1 us | **67 ns** single-thread, 227 ns under 4-thread contention; 127 ns contended at P = 0.01 (`collector_bench`) |
| inference, per compaction | 1-5 ms (Treelite) | 1.9-2.3 us per range × ~100-300 ranges = **0.2-0.7 ms** |
| overlap check | 1-3 ms | inside the inference figure above; not separately visible |
| warming, per block | — | 10.8 us (E2), i.e. one `pread` of a 4 KiB block |
| collector memory | 3-16 KB | ~64 KB for 1,024 ranges after the sizing fix (was 460 MB) |

Sampling halves the contended collector cost (227 → 127 ns), which is the
direction of the paper's Table 1 without being the same measurement.

## F2 — skew sweep completed at 0.99

| zipf | LRU | WarmAll | Leaper (prefetch only) | Leaper vs LRU |
|---|---|---|---|---|
| 0.99 | 37.51% | 49.38% | 47.88% | **+10.37pp** |

(Flush-path warming only — superseded; re-measured in G4.)

## F3 — SSAD with a relative threshold: still not a test

Relative mode suspended for 175 of 180 s again, for a different reason: the
harness fed `set_health(0.0)` during warmup, seeding the EWMA at zero, so the
first real miss ratio tripped it. That is fixed. More fundamentally, a workload
that is *uniformly* different from training is not a change over time, and a
change detector cannot be evaluated on it. The harness now switches the
workload's shape mid-run (`--shift_at_s`), and SSAD is tested against that in
G5.

## F4 — prefetch precision without censoring

| policy | prefetched | read at least once | precision |
|---|---|---|---|
| WarmAll | 119,003 | 22,353 | 0.188 |
| Oracle (1-interval) | 2,515 | 2,515 | 1.000 |
| Leaper | 0 of 21,217 warm calls landed | — | — |

The Leaper row is what exposed the defect above.

## H — the corrected measurements

Tags `_v3`: compaction-path warming works (fifth defect) and the hit ratio
counts the workload threads only (sixth defect). Same protocol as M4: models
trained on seed 42, every policy evaluated on seed 1234 from a freshly
filled database. Three things to keep in mind when reading the tables:

* **EagerEvict is the floor for every warming policy.** All policies except
  stock reclaim the block cache entries of deleted SSTs, so the value of
  *warming* is a policy's distance from EagerEvict, not from LRU.
* **Warming is done by the compaction thread**, one emulated read (200 us on
  "slow storage", ~10 us on NVMe) per block, before the compaction moves on.
  The `comps` column is how many compactions finished in the measured window;
  when it drops, the warm reads are why. Section H11 charges the same cost to
  a separate thread instead, which brackets it from the other side.
* **`bg lk`** is the share of all block cache lookups made by LevelDB's own
  background thread — the part the sixth defect was counting.

### H1 — slow storage, 64 MB cache (the paper's regime)

200 us per read, 8,000 ops/s, 75/20/5 read/update/insert, 180 s.

| policy | hit ratio | vs LRU | vs EagerEvict | p99 us | comps | blocks warmed | read at least once |
|---|---|---|---|---|---|---|---|
| LRU | 45.69% | — | — | 282 | 59 | — | — |
| EagerEvict | 46.54% | +0.85pp | — | 287 | 59 | — | — |
| IncrementalWarmup | 42.98% | -2.71pp | -3.56pp | 298 | 41 | 273k | 4.5% |
| WarmAll | 43.27% | -2.42pp | -3.27pp | 296 | 36 | 340k | 3.2% |
| WarmFlushOnly (= RocksDB kFlushOnly) | **47.01%** | **+1.32pp** | +0.47pp | 291 | 59 | 7.8k | 2.2% |
| Leaper (both phases) | 46.29% | +0.59pp | -0.25pp | 337 | 56 | 43k | 11.6% |
| Leaper (prefetch only) | 46.56% | +0.87pp | +0.02pp | 292 | 58 | 37k | 12.1% |
| Oracle (W=1) | 46.02% | +0.33pp | -0.52pp | 325 | 50 | 153k | 10.7% |

"Blocks warmed" and the precision are for the measured window only; the
policies also warm during the database fill and the warmup, and those blocks
are never read (WarmAll: 66k of its 406k cumulative).

In the paper's own terms (miss rate inside "during and after compaction"
windows, T2 = 2 s): stock 54.70%; EagerEvict eliminates 2.0% of those misses,
Leaper (prefetch only) 1.8%, the oracle 0.5%, WarmAll adds 4.8%. No policy
produced a latency spike (p95 > 2x median) in this regime.

Read against the floor, **warming buys nothing here**: Leaper's prefetch is
exactly neutral, the oracle's costs half a point, and warming everything costs
three. The reason is in the last three columns. A 64 MB cache holds ~16k
blocks; WarmAll pushes 340k through it in 180 s, of which 3.2% are ever read,
and each of those 340k reads is 200 us on the compaction thread, which is why
it finishes 36 compactions where stock finishes 59. Leaper warms 9x fewer
blocks at 4x the precision and so does no damage — but 12% precision is not
enough to gain anything either, because the block it warms is usually evicted
by the time its range is read.

The best policy in this regime warms almost nothing: **flush outputs only**,
7.8k blocks in 180 s, 2.2% of them read — but the ones that are read are the
keys the workload just wrote, i.e. the hottest blocks there are, each hit
dozens of times. It is +0.45pp over Leaper (prefetch only), above the ~0.3pp
run-to-run noise (H9) but not by much, and it costs nothing: no model, no
collector, and no compaction slowdown. This is what RocksDB ships as
`prepopulate_block_cache=kFlushOnly`, and it is the same policy that the
fifth defect had silently reduced "WarmAll" to in every measurement before
this section — which is why "WarmAll" used to look so strong.

### H2 — NVMe, 128 MB cache

No emulated delay, 40,000 ops/s, 300 s.

| policy | hit ratio | vs LRU | vs EagerEvict | p99 us | comps | prefetch precision | spikes |
|---|---|---|---|---|---|---|---|
| LRU | 83.34% | — | — | 31 | 252 | — | 0 |
| EagerEvict | 83.68% | +0.33pp | — | 30 | 252 | — | 0 |
| IncrementalWarmup | 84.07% | +0.72pp | +0.39pp | 31 | 254 | 0.218 | 0 |
| WarmAll | 83.48% | +0.14pp | -0.19pp | **55** | 252 | 0.144 | **30** |
| WarmFlushOnly | 83.69% | +0.34pp | +0.01pp | 29 | 253 | 0.085 | 0 |
| Leaper (both phases) | 86.63% | +3.28pp | +2.95pp | 32 | 253 | 0.744 | 0 |
| Leaper (prefetch only) | **86.57%** | **+3.23pp** | +2.90pp | 30 | 254 | 0.731 | 0 |
| Oracle (W=1) | 88.79% | +5.45pp | +5.12pp | 28 | 253 | 0.883 | 0 |

Misses inside compaction windows: Leaper eliminates 6.1% of stock's, the
oracle 16.6%, WarmAll adds 1.4% and is the only policy with latency spikes.

This is the paper's result, in this setup: **selection beats both warming
everything and warming nothing, by about three points of hit ratio, with an
oracle showing two more points of headroom.** Warm reads are cheap here, so
compaction throughput is untouched by any policy (`comps` 252-254); what
separates the policies is purely what they put in the cache, and Leaper's
prefetched blocks are read 73% of the time against WarmAll's 14%. Flush-only
warming, the winner on slow storage, is worth nothing here: the cache is
large enough that the just-written keys are still resident from their own
writes' reads.

Whether the difference from H1 is the device or the cache size is the
question H10 answers.

### H3 — oracle lookahead, slow storage

| lookahead W | hit ratio | comps |
|---|---|---|
| 1 (the paper's prediction target) | 46.20% | 56 |
| 5 | 45.98% | 51 |
| 20 | 44.65% | 45 |

Before the sixth defect was fixed this table read the other way round
(38.68 / 38.96 / 39.19), and "selection needs twenty intervals of foresight"
was a conclusion of this document. It was the compaction-slowdown artefact:
the more the oracle warms, the fewer compactions complete, the more compaction
reads leave the denominator. Counted correctly, warming more of the future
hurts, for the same reason WarmAll hurts in H1.

### H4 — zipf sweep, slow storage, 64 MB cache

| zipf | LRU | WarmAll | Leaper (prefetch only) | Leaper warms / WarmAll warms |
|---|---|---|---|---|
| 0.0 | 13.97% | 12.17% | 12.51% | |
| 0.3 | 16.20% | 14.32% | 14.31% | |
| 0.5 | 23.11% | 20.48% | 20.47% | 57% |
| 0.9 | 51.37% | 46.62% | 47.04% | |
| 0.99 | 58.98% | 54.30% | 53.86% | 63% |

Every prefetching policy loses to LRU at every skew, by 1.5-5pp. On a
stationary zipfian LRU already holds the hot set, and the last column says
why Leaper does not help it: with 40,000-key ranges over 4M keys there are
100 ranges, 6,000 reads/s reach nearly all of them every second, so nearly
every range is labelled hot and Leaper warms 60% of what WarmAll warms — at
3.9% precision (zipf 0.99: 207,600 warmed, 8,159 read). This is the range
granularity problem from M1 in its purest form: the ranges Algorithm 1's
floor produces are far coarser than a zipfian hot set. The paper's Figure
13(a) shape (gain rising with skew) does not reproduce under corrected
counting; what rises with skew is LRU.

### H5 — distribution shift and SSAD, slow storage

Workload changes at t = 120 s from 16 chains / 8 s lifetimes to 64 chains /
3 s; the model was trained on the first shape only. SSAD relative
threshold 0.3, window 5.

| policy | whole run | before shift | after shift | SSAD suspended |
|---|---|---|---|---|
| LRU | 34.59% | 39.73% | 24.37% | — |
| WarmAll | 31.96% | 37.47% | 20.99% | — |
| Leaper (prefetch only) | 34.90% | 40.42% | 23.93% | — |
| Leaper (prefetch only) + SSAD | **35.18%** | 40.45% | **24.66%** | 60 s (all of the post-shift period) |

Two firsts. SSAD *fired* on a real change — the miss ratio, now counted
correctly, jumped 26% relative — and suspending the prefetcher after the
shift was worth +0.73pp over leaving it running with a stale model (24.66 vs
23.93 after the shift), which turns Leaper's post-shift -0.44pp against LRU
into +0.29pp. The effect is small and close to the noise floor (H9); it is
the direction the paper claims, at a size the paper does not quantify.

### H6 — range queries, slow storage

10% of reads are 32-record scans.

| policy | hit ratio | vs LRU | misses in windows vs stock | prefetch precision |
|---|---|---|---|---|
| LRU | 50.70% | — | — | — |
| WarmAll | 49.46% | -1.24pp | +3.5% | 0.035 |
| Leaper (prefetch only) | 51.78% | +1.08pp | -2.0% | 0.154 |
| Oracle (W=1) | 51.93% | +1.23pp | -1.8% | 0.124 |

Same ordering as H1 with slightly larger margins; scans give the prefetched
block more chances to be read before it is evicted.

### H7 — SSAD at a 10% threshold

Same shift as H5, relative threshold 0.1: whole run 35.46%, before the shift
40.74%, after it **24.98%**, suspended for the whole post-shift minute — the
same suspension as at 0.3, so the same result within noise (+0.32pp on the
post-shift period, +0.28pp whole-run). On this shift the threshold does not
matter between 0.1 and 0.3; both fire within the window.

### H8 — run-to-run noise, same seed

Three rows of H1 repeated with an identical binary, seed and protocol:

| policy | run 1 | run 2 | difference |
|---|---|---|---|
| LRU | 45.69% | 45.54% | 0.15pp |
| WarmAll | 43.27% | 43.55% | 0.28pp |
| Leaper (prefetch only) | 46.56% | 46.52% | 0.04pp |

And the same three rows of H2 (NVMe, 128 MB):

| policy | run 1 | run 2 | difference |
|---|---|---|---|
| LRU | 83.34% | 83.32% | 0.02pp |
| WarmAll | 83.48% | 83.49% | 0.01pp |
| Leaper (prefetch only) | 86.57% | 86.75% | 0.18pp |

Compaction counts also repeat (59/58, 36/33, 58/58 on slow storage; 252/252,
252/249, 254/249 on NVMe). **Differences under about 0.3pp in the
slow-storage tables, and about 0.2pp on NVMe, are not differences.** That covers
Leaper-vs-EagerEvict in H1 (0.02pp) and H5's Leaper-vs-LRU (0.31pp); it does
not cover WarmAll's -2.4pp, WarmFlushOnly's +0.47pp over EagerEvict, SSAD's
+0.73pp on the post-shift period, or anything in H2.

### H10 — is it the device or the cache? Cache size on both devices

Same four policies, same models as H1/H2; only the cache size moves. Gains
are given against EagerEvict, the floor for warming (H8's noise is ~0.3pp).

| device, cache | LRU | EagerEvict | WarmAll vs floor | Leaper (prefetch only) vs floor | Leaper precision | WarmAll precision | comps LRU → WarmAll |
|---|---|---|---|---|---|---|---|
| slow, 64 MB (H1) | 45.69% | +0.85pp | **-3.27pp** | +0.02pp | 0.12 | 0.03 | 59 → 36 |
| slow, 128 MB | 55.91% | +3.62pp | **-3.67pp** | -0.15pp | 0.42 | 0.11 | 60 → 36 |
| slow, 256 MB | 67.94% | +2.36pp | **+7.03pp** | +2.65pp | 0.70 | 0.32 | 60 → 37 |
| NVMe, 64 MB | 72.27% | +1.12pp | **-5.81pp** | +0.37pp | 0.44 | 0.05 | 254 → 254 |
| NVMe, 128 MB (H2) | 83.34% | +0.33pp | -0.19pp | **+2.90pp** | 0.73 | 0.14 | 252 → 252 |

Three regimes, and neither device nor cache size alone separates them:

* **Cache smaller than the working set** (64 MB on either device, 128 MB on
  slow storage): nothing a prefetcher does helps. Leaper is neutral — its
  precision rises with the cache (0.12 → 0.44) but the block it warms is
  still gone before its range is read often enough to matter — and WarmAll
  loses 3-6 points because every junk block it inserts displaces a block
  that would have been hit. On NVMe this happens with compaction throughput
  untouched (254 → 254), so it is pure displacement, not the slowdown seen
  on slow storage.
* **Cache about the size of the working set** (NVMe, 128 MB): selection
  pays, +2.9pp over the floor with 73% precision, while warming everything
  is neutral. This is the paper's regime and the paper's result.
* **Cache larger than the working set** (slow, 256 MB): everything pays, and
  warming everything pays most — +7.0pp against Leaper's +2.65pp — because
  junk no longer displaces anything and what limits a selective policy is
  recall, not precision: Leaper warms 40k blocks at 70% precision where
  WarmAll warms 358k at 32%, and 32% of 358k is 115k useful blocks against
  Leaper's 28k.

What still separates slow-128 MB (Leaper -0.15pp) from NVMe-128 MB (+2.9pp)
is not the device's latency: the two runs differ in operation rate (8,000 vs
40,000 per second) and write rate, so a hot range on NVMe receives five times
as many reads during its 8 s lifetime, which is five times as many chances
for a prefetched block to be hit before it is evicted. H14 varies the
lifetime directly to test that.

### H11 — charging the warm reads to a separate thread

Everything above warms from the compaction thread, so on slow storage every
warmed block costs the compaction 200 us. `--warm_async=1` moves the reads
to a dedicated thread; under an emulated per-read delay that makes them free
(no device queue to contend on), which is the opposite bound. Slow storage,
64 MB cache, same runs as H1:

| policy | warm on compaction thread (H1) | comps | warm on its own thread | comps | p99 us (sync → async) |
|---|---|---|---|---|---|
| WarmAll | 43.27% | 36 | **42.37%** | 56 | 296 → 557 |
| Leaper (prefetch only) | 46.56% | 58 | 46.51% | 57 | 292 → 346 |
| Oracle (W=1) | 46.02% | 50 | 46.96% | 59 | 325 → 414 |

Compaction throughput comes back (36 → 56 for WarmAll) and the verdict does
not change. WarmAll gets *worse* with free reads, because more compactions
now finish and it warms 670k blocks instead of 475k through a 16k-block
cache; its p99 nearly doubles from the displacement. Leaper is unchanged.
The oracle gains 0.9pp — the part of its H1 deficit that was the slowdown —
and ends +0.4pp over EagerEvict, which is about the noise floor. On a cache
smaller than the working set, **the cost of warming is displacement, and no
accounting for the read makes it go away.** WarmAll's real-device cost lies
between these two rows: the reads would neither stall compaction outright nor
be free, but queue against the workload's own.

The same check on the zipf 0.99 workload (H4), where both prefetchers lose
5pp to LRU:

| policy | warm on compaction thread | comps | warm on its own thread | comps | p99 us |
|---|---|---|---|---|---|
| LRU | 58.98% | 35 | — | — | 408 |
| WarmAll | 54.30% | 21 | 53.36% | 36 | 443 → 286 |
| Leaper (prefetch only) | 53.86% | 23 | 53.26% | 36 | 362 → 287 |

Same picture: the hit ratio drops a further 0.6-0.9pp once the warms are
free, and the tail latency improves because compaction no longer stalls —
two different costs, and the one that decides the hit ratio is the one that
does not depend on where the read runs.

### H12 — RocksDB, workload-only counting (m7v3)

NVMe, 128 MB block cache, 40,000 ops/s, 300 s; same lifecycle workload as H2.

| policy | hit ratio | vs stock | p99 us | comps |
|---|---|---|---|---|
| stock (`kDisable`) | 81.86% | — | 28 | 337 |
| `kFlushOnly` | 81.87% | +0.01pp | 31 | 335 |
| `kFlushAndCompaction` | 81.79% | -0.07pp | 27 | 337 |
| Leaper (predict at begin, warm at end) | 81.92% | +0.06pp | 30 | 337 |
| Leaper + 32 MB row cache | 79.32% | -2.54pp | 35 | 336 |

Nothing moves — the same null result as before the counting fix, now with
the fix ruling out the listener's own warm scans (93k background lookups,
counted separately) as the cause. RocksDB's background share is only 10% of
lookups and 80% of those hit, so the sixth defect never mattered much here.

The null is structural, not a verdict on the method. On LevelDB the hook
warms *every output block* of the compaction that overlaps a predicted-hot
range — 437k blocks in the H2 run, 73% of them read. The zero-patch RocksDB
adapter has no block-level access to the compaction's output; it warms by
seeking a DB iterator to each hot range and scanning the first
`warm_scan_keys` = 4,096 keys of its 40,000. Over the run that was 660 range
scans, roughly 80k blocks — a fifth of the LevelDB volume, and only the
first tenth of each range while the hot keys are spread across it. The row
cache result is the same story one level up: a 32 MB row cache taken out of
a 128 MB budget holds fewer bytes than the blocks it displaces.

### H13 — RocksDB with the warm scan covering the whole range

| warm scan per hot range | hit ratio | vs stock | background lookups |
|---|---|---|---|
| 4,096 keys (H12) | 81.92% | +0.06pp | 1.26M |
| 12,000 keys | 82.01% | +0.15pp | 1.44M |
| 40,000 keys (the whole range) | 82.23% | +0.37pp | 2.09M |

Scanning the whole range warms ten times as much and buys 0.3pp — above
RocksDB's noise, and a tenth of what the same model earns on LevelDB in the
same regime. The rest of the gap is what the iterator warms *besides* the
compaction's output: a merging iterator over a 40,000-key range reads the
data blocks of every level that holds a version of those keys, so most of
what it pulls in is older-level blocks that were cold for a reason, and it
has no way to touch only the file the compaction just wrote. The LevelDB hook
warms exactly those output blocks and nothing else (73% of them read); the
RocksDB adapter cannot, without either a patch to `CompactionJob` or a way
to open the output SST under the same cache keys the table reader will use.
That, not the model, is what a faithful RocksDB port still needs.

### H14 — how long a hot range stays hot

H10 left one variable between slow-128 MB (Leaper neutral) and NVMe-128 MB
(Leaper +2.9pp): reads per hot lifetime. Here the lifetime moves and nothing
else does; the model is retrained for each workload (its own seed-42 run).

Slow storage, 128 MB cache, lifetimes 8 s (H10) → 40 s:

| policy | lifetime 8 s | lifetime 40 s | vs floor at 40 s | precision at 40 s |
|---|---|---|---|---|
| LRU | 55.91% | 78.84% | — | — |
| EagerEvict | +3.62pp | +0.99pp | — | — |
| WarmAll | -0.05pp | +3.37pp | +2.38pp | 0.19 |
| Leaper (prefetch only) | +3.47pp | **+7.90pp** | **+6.91pp** | 0.74 |

Five times the reads per lifetime turns Leaper's prefetch from neutral into
+6.9pp over the floor and +4.5pp over WarmAll — on slow storage, with the
warm reads still on the compaction thread (52 compactions against stock's
56), and with the same 128 MB cache that gave nothing at 8 s. Precision goes
from 0.42 to 0.74 because a warmed block now has forty seconds of demand to
meet instead of eight. **The regime that decides whether selection pays is
reads per hot lifetime against cache size**, not the device: slow storage
was never the problem, the 8 s lifetime at 8,000 ops/s was.

And the reverse on NVMe, 128 MB cache, lifetimes 8 s (H2) → 2 s:

| policy | lifetime 8 s | lifetime 2 s | vs floor at 2 s | precision at 2 s |
|---|---|---|---|---|
| LRU | 83.34% | 64.57% | — | — |
| EagerEvict | +0.33pp | +0.78pp | — | — |
| WarmAll | +0.14pp | +0.73pp | -0.05pp | 0.10 |
| Leaper (prefetch only) | +3.23pp | +1.91pp | +1.13pp | 0.27 |

A quarter of the reads per lifetime cuts Leaper's gain over the floor from
+2.9pp to +1.1pp and its precision from 0.73 to 0.27; the ordering survives
(selection still beats warming everything, which is neutral) but most of
the value is gone. Put together with the row above, the two runs move the
same knob in opposite directions from the two H10 configurations and get the
opposite results, which is as close to isolating the variable as this
harness can get.

**What this means for the paper's claim.** The paper measured Leaper on
production e-commerce and messaging traffic whose hot ranges — a sale, a
conversation — stay hot for minutes to hours against a cache that holds
them. That is the top-right corner of this map, where selection pays
several points over warming everything and warming everything pays over
LRU. The synthetic workload that this repository started from (8 s
lifetimes at 8,000 ops/s on a 64 MB cache) is the bottom-left corner, where
nothing pays, and the early conclusion that "WarmAll dominates" was a
statement about that corner, measured with two defects on top. Neither
corner is the whole picture; the map is.

### H15 — RocksDB: the null result explained, and block-level warming without a patch

Three things were wrong on the RocksDB side, found in this order.

**The seventh defect — concurrent jobs shared one prediction.** RocksDB runs
flushes and compactions on separate background threads
(`max_background_jobs = 2`); LevelDB has one. The adapter kept a single
`pending_` list of the ranges chosen at a job's Begin and consumed it at the
next End — whichever job's End came first — and the core's own per-job
prediction (`hot_t2_`) could be overwritten by a concurrent Begin before the
first job had finished choosing. Fixed by keying the chosen list by RocksDB's
job id and serialising predict-and-choose. Real, but it turned out not to be
what was holding the result down: with the fix, the H12 method moves from
81.92% to 81.93%.

**Block-level warming without a patch.** RocksDB's block cache key is derived
from the SST's own properties (`db_session_id`, `orig_file_number`;
`BlockBasedTable::SetupBaseCacheKey`), so a reader that opens one of the DB's
files with the DB's table factory shares its cache *and* its keys.
`sst_warm_check` proves it end to end: warm a range through an
`SstFileReader`, `Get` the same keys through the DB with `PerfContext` on,
zero block reads from the file. The adapter's `--warm_mode=sst` opens each
output file the job reports (`CompactionJobInfo::output_files`,
`FlushJobInfo::file_path`) and reads only the blocks inside a predicted-hot
range — what the LevelDB hook does, with no change to RocksDB. On the H12
configuration it warms 34.7k blocks from 98 files and lands at 82.01%
(+0.15pp): the mechanism works, and it still does not matter.

**The actual cause: there was almost nothing to prefetch for.** The RocksDB
harness left `max_bytes_for_level_base` at RocksDB's default of 256 MB;
LevelDB's L1 is hard-coded to 10 MB. On a 480 MB database that difference
is the difference between **5 compactions and 254** in the same 300 s under
the same writes (RocksDB's `LOG`: 91 flushes, 5 compactions, each ~0.25 s).
Flushes do not invalidate anything — they create files — so the cache
invalidation the paper is about occurred five times in five minutes, and
the inference count confirms it: 22.7k inferences over the run is ~50 jobs
of 101 candidate ranges, against ~350 jobs on LevelDB. All RocksDB
"engine comparison" numbers before this point (E1, H12-H13) compared
LevelDB to a RocksDB that was hardly compacting. The bench now takes
`--level_base_mb` and `--l0_trigger`; with `level_base_mb=10` RocksDB has
LevelDB's tree shape.

Same regime as H12 (NVMe, 128 MB, 300 s), RocksDB with `level_base_mb=10`
and the job-id fix; the Leaper rows use the m7v3 models:

| policy | hit ratio | vs stock | compaction writes in window |
|---|---|---|---|
| stock | 82.68% | — | 656 MB |
| `kFlushOnly` | 82.72% | +0.04pp | |
| `kFlushAndCompaction` | 82.44% | -0.24pp | |
| Leaper, DB-iterator warming (4,096 keys per range) | 82.73% | +0.05pp | |
| Leaper, block-level `sst` warming (66,947 blocks from 225 files, 0 failures) | 82.83% | +0.14pp | 653 MB |

Still nothing, and the last column says why the level size did not help
either. In the same 300 s, under the same 22 flushes of the same writes:

| engine | compactions | compaction output | flush output |
|---|---|---|---|
| LevelDB (H2 stock run) | 261 | **9,556 MB** | 139 MB |
| RocksDB (`level_base_mb=10`, stock run) | 49 | **672 MB** | 126 MB |

**LevelDB rewrites fourteen times as much data as RocksDB on this workload,
so it invalidates fourteen times as much cache.** The first suspect was
RocksDB's dynamic level sizing (`level_compaction_dynamic_level_bytes`, on
by default since 8.4, which is why the output levels in its log are 4-6);
H16 turns it off and the volume does not change. The difference is in how
the two pickers move data. RocksDB's leveled compaction here runs at a write
amplification of 3-5 under either level policy. LevelDB's, with a 10 MB L1
under writes spread across the whole key space, spends 239 of its 261
compactions moving one 4 MB L1 file at a time into the ~40 MB of L2 it
overlaps (median compaction 41 MB, 9,355 of the 9,556 MB), for a write
amplification near 70. The cache invalidation problem the paper describes is
proportional to that number; on RocksDB it is an order of magnitude smaller
than on LevelDB for this workload, and there is correspondingly little for
any prefetcher to recover. That — not the adapter, and not the model — is
what the RocksDB null result measures. The engine comparison this repository
set out to make turns out to be a comparison of compaction volumes first.

### H16 — RocksDB with classic leveling

`dynamic_level_bytes=0`, `level_base_mb=10`, L0 trigger 4; otherwise H15.

| policy | hit ratio | vs stock | compaction writes in window |
|---|---|---|---|
| stock | 81.73% | — | 452 MB |
| `kFlushOnly` | 81.66% | -0.07pp | 446 MB |
| `kFlushAndCompaction` | 81.55% | -0.18pp | 438 MB |
| Leaper, block-level `sst` warming (50,453 blocks, 171 files, 0 failures) | **81.86%** | +0.13pp | 450 MB |

Same volume as with dynamic levels, same ordering, same size of effect:
Leaper's block-level warming is the best of the four every time it is run
on RocksDB (+0.13 to +0.15pp, with `kFlushAndCompaction` at -0.18 to
-0.24pp), and every time the margin sits at RocksDB's ~0.2pp noise floor,
because ~450-670 MB of compaction per five minutes against a 128 MB cache
is not much invalidation. The mechanism is now the same on both engines and
verified on both; what a RocksDB result *with* a margin needs is a
configuration that compacts as much as LevelDB does — a database much
larger than its level targets, heavier writes, or universal compaction —
which is the next experiment, not a fix.

### H17 — RocksDB at the paper's scale

H15/H16 ran RocksDB on a 480 MB database, which is not what the paper
measures. Section 7.3 of the paper uses 10 GB of data, 200-byte records and
a 4 GB buffer cache of which 3 GB is block cache — a 30% cache-to-data
ratio — and reports "about 10 background operations" in a 200 s test. Neither
that paper nor the X-Engine paper gives compaction bytes or a write
amplification figure (X-Engine reports only that reusing 2 MB extents cuts
write amplification 63% against RocksDB, and concedes that compactions still
hurt hit rates on a regular basis), so the quantity to match is the shape of
the setup, not a number.

`experiments/run_m7_paperscale.sh` puts RocksDB there: 50M records of 200 B
(10 GB), a 3 GB block cache, 200 s measured. Getting into the paper's regime
took three corrections, and each failure is informative.

**First attempt: no compaction at all.** At the paper's write rate (4,000
writes/s = 141 MB in 200 s) against a 64 MB write buffer and an L0 trigger
of 4, RocksDB needs 256 MB of writes before its first L0→L1 compaction. It
performed **zero compactions in 200 s** — 168 MB of flushes and nothing
else. On a 10 GB database RocksDB simply does not move data at that write
rate, which is the H15 write-amplification finding in its starkest form.
Fix: a 16 MB write buffer and 15,000 writes/s, giving ~2.7 GB of compaction
writes per 200 s.

**Second attempt: the working set did not fit.** With compaction volume
fixed, stock hit ratio was 67.6% — the H10 regime where nothing pays, and
the matrix says exactly that:

| policy | hit ratio | vs stock |
|---|---|---|
| stock | 67.58% | — |
| `kFlushOnly` | 66.99% | -0.59pp |
| `kFlushAndCompaction` | 66.70% | -0.88pp |
| Leaper (block-level `sst` warming) | 67.59% | +0.01pp |

The cause was not cache size but hot-set churn: with 8 s lifetimes a new
generation of 64 ranges becomes hot every 8 s, so a large share of reads are
first touches that no prefetcher can anticipate. The paper's workloads have
a stable hot set — it reports a 99% hit ratio in the absence of background
operations.

**Third attempt: the paper's regime.** Same 10 GB, same 3 GB cache, same
2.7 GB of compaction per 200 s, hot ranges living 60 s instead of 8. Stock
hit ratio 90.1%, model precision 0.999 and recall 0.96 over 507 ranges:

| policy | hit ratio | vs stock | p99 us | blocks warmed |
|---|---|---|---|---|
| stock | 90.10% | — | 41 | — |
| `kFlushOnly` | 91.49% | +1.39pp | 78 | |
| `kFlushAndCompaction` | 91.40% | +1.30pp | 85 | |
| Leaper, block-level `sst` warming | **91.65%** | **+1.55pp** | 73 | 271,440 from 471 files |

**Warming pays here, and this is the first RocksDB configuration in which it
does.** Every policy that warms gains over a point; Leaper is the best of
them and has the lowest tail of the three, but its margin over warming
flush outputs alone is 0.16pp. Repeating all three runs with the same seed
puts that margin well outside the noise — this configuration is the most
repeatable in the repository:

| policy | run 1 | run 2 | difference |
|---|---|---|---|
| stock | 90.10% | 90.11% | 0.01pp |
| `kFlushOnly` | 91.49% | 91.48% | 0.01pp |
| Leaper | 91.65% | 91.65% | 0.00pp |

(Tail latency is not repeatable at that resolution: stock's mean p99 was
41 us in one run and 75 us in the other, so the p99 column above ranks
policies within a run and should not be compared across runs.)

So on RocksDB, at the paper's scale and in the paper's regime, **selection
beats both warming nothing (+1.55pp) and the best naive warming
(+0.16pp)**, and the second margin is sixteen times the noise floor but a
tenth of the first.

What this says about the reproduction: the paper's regime is reachable on
RocksDB, and in it the paper's qualitative claim holds — prefetching what
compaction invalidates recovers cache misses that LRU cannot. What does not
reproduce is the *size* of the advantage a learned prefetcher has over the
naive alternatives, which on RocksDB stays a fraction of a point in every
configuration tried, against 2.9pp on LevelDB in the equivalent regime. The
two engines differ in how much they invalidate, and that difference decides
how much there is to win.

## The eighth defect — the RocksDB warm paths had no budget

The core enforces a prefetch budget in `ShouldPrefetch`, by adding up
`BlockRef::size` and refusing once a run of warms exceeds
`max_prefetch_frac x cache_bytes`. The RocksDB adapter's candidates are whole
key ranges with `size = 0`, because RocksDB gives a plug-in no block layout to
predict over, so that check never fired: the budget was a no-op on RocksDB
from the start. It did not matter while the workloads had ~100 coarse ranges
and a job warmed two of them. At 25,000 ranges it matters enormously — one
200 s run below spent **3,350 seconds of background time** warming, scanning
18.2M ranges through DB iterators.

Fixed in the adapter, where the warming actually happens: both warm paths now
count the data blocks they really pull in, using the thread-local
`PerfContext` the harness already enables, and stop once a job has warmed
`max_prefetch_frac x cache_bytes` worth. Verified on a smoke run: the
iterator mode now warms 939 of 4,510 predicted-hot ranges and stops; the
`sst` mode is unaffected in practice because the output files bound it
already.

## H18 — the paper's two real workloads, in stationary form

H17 got RocksDB to the paper's scale but kept this repository's lifecycle
workload, whose hot ranges are born and die. The paper's two real workloads
are stationary power laws over a fixed table (Table 2: e-commerce is zipf 0.3
with a 6:1 read/write ratio over 10m rows, instant messaging zipf 0.9 with
2:3 over 8m rows). Four configurations, all on RocksDB with block-level
`sst` warming, 200 s measured, 60,000 ops/s.

Two of them use a 10 GB table against the 3 GB cache, matching Section 7.3's
stated data size; two use the tables' own sizes from Table 2, where the whole
table fits in the cache.

| | table | cache : data | read/write | compaction in 200 s | stock hit ratio |
|---|---|---|---|---|---|
| A | 50m rows, 10 GB | 0.3 | 40/60 | 5.8 GB | 54.64% |
| B | 50m rows, 10 GB | 0.3 | 85/15 | 1.2 GB | 34.48% |
| C | 8m rows, 1.5 GB | 2.0 | 40/60 | 6.9 GB | 70.09% |
| D | 10m rows, 1.8 GB | 1.7 | 85/15 | 1.9 GB | 95.40% |

Results, as points of block cache hit ratio over stock:

| policy | A | B | C | D |
|---|---|---|---|---|
| `kFlushOnly` | +1.82 | +0.10 | +8.30 | +0.93 |
| `kFlushAndCompaction` | +6.47 | +0.34 | **+19.23** | **+4.23** |
| Leaper, block-level | **+8.71** | +0.25 | +15.48 | +4.23 |

And the model that produced them, on each workload:

| | ranges | positive rate | LightGBM precision / recall | AUC |
|---|---|---|---|---|
| A | 25,000 | 0.279 | 0.748 / 0.336 | 0.737 |
| B | 25,000 | 0.843 | 0.843 / 1.000 | 0.592 |
| C | 4,000 | 0.765 | 0.765 / 1.000 | 0.738 |
| D | 5,000 | **1.000** | 1.000 / 1.000 | 0.656 |

**The two tables explain each other.** Selection can only pay where the
prediction target carries information, and on a stationary power law it
usually does not: at 24,000-51,000 reads per second, almost every key range
is touched in any given second, so "will this range be read in the next
interval" is true nearly everywhere. In D it is true *everywhere* — the
positive rate is exactly 1.000, the model predicts every range hot, and
Leaper's hit ratio is identical to warming everything to four decimal places
(99.62% both). In B the rate is 0.843 and the model is barely above chance
(AUC 0.592). Only in A, where a 10 GB table is read at 24,000/s through
2,000-key ranges, do 72% of ranges go untouched in a second — and there
Leaper beats warming everything by 2.24pp, the largest margin it achieves
anywhere on RocksDB.

C is the opposite corner and the H10 regime map's prediction, confirmed at
the paper's own workload size: the table fits in the cache, compaction churns
6.9 GB through it, and recall beats precision, so warming everything wins by
3.75pp. Leaper is not useless there — it is +15.48pp over stock, and its p99
is the lowest of the four policies (31 us against stock's 57 and
`kFlushAndCompaction`'s 106), because it does a fifth of the warming I/O.
Whoever runs this configuration is choosing between hit ratio and tail
latency, not between a good policy and a bad one.

**A caveat that applies to every RocksDB number in this document.**
`kFlushAndCompaction` inserts each block into the cache as the table builder
produces it, from memory, at no I/O cost. The plug-in has to re-read the
finished file. So the comparison is not selection versus no selection; it is
selection-plus-a-re-read versus no-selection-and-no-read. A faithful
implementation of the paper's design would put the selection *inside*
`prepopulate_block_cache`, warming from memory only the blocks whose range is
predicted hot, and would have Leaper's precision at RocksDB's cost. That
needs a patch to `BlockBasedTableBuilder`, and it is the single most
worthwhile piece of work left in this repository.

**What this says about the paper's workloads.** The paper reports its models
at precision and recall around 0.95 on the real Tmall and DingTalk traces, so
on that data the label is clearly informative — many ranges really are cold
in a given interval. Our synthetic power laws cannot reproduce that at these
read rates, which is a limitation of the generator, not evidence against the
paper. What the four configurations do establish is the *shape* of the
answer: the learned part of Leaper earns its keep exactly where a large
fraction of key ranges are cold at any moment and the cache cannot hold
everything, and it degenerates gracefully to "warm everything" where that is
not true.
