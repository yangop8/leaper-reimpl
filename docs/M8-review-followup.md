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
