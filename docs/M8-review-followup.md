# M8 — Review follow-up: defects, missing pieces, and verifying the paper on its own terms

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

Compaction counts also repeat (59/58, 36/33, 58/58). **Differences under
about 0.3pp in the slow-storage tables are not differences.** That covers
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

(Amended after H13, below, which raises the scan to the whole range.)
