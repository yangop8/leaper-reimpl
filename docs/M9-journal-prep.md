# M9 — Toward the journal version: selective prepopulate on RocksDB, and the FAST'20 workload model

Two of the five items identified as necessary before this work can be
written up for a journal (the others: a dedicated machine for overhead and
latency, multi-seed variance, and a decision on phase 1).

## 1. Selective prepopulate: the paper's mechanism, on RocksDB, at RocksDB's cost

Every RocksDB comparison in M8 had an asymmetry. RocksDB's own
`prepopulate_block_cache` inserts each output block into the block cache from
memory, at the moment the table builder produces it, at no I/O cost; the
zero-patch adapter could only warm by re-opening the finished file and reading
the chosen blocks back (`--warm_mode=sst`). So "Leaper" there meant
selection plus a re-read, against a built-in that neither selects nor reads.

The paper's design is the former with selection: "once a new block is full,
it checks overlap with hotT2 and we determine whether it should be prefetched"
(Section 6). RocksDB has the insertion point; what it lacks is the decision.
The patch in [`adapters/rocksdb/rocksdb-11.8-prepopulate-filter.patch`](../adapters/rocksdb/rocksdb-11.8-prepopulate-filter.patch)
(43 lines, two files) adds

```cpp
class PrepopulateBlockFilter {
  virtual bool ShouldWarm(TableFileCreationReason reason,
                          const Slice& first_internal_key,
                          const Slice& last_internal_key) = 0;
};
std::shared_ptr<PrepopulateBlockFilter> prepopulate_block_filter;  // in BlockBasedTableOptions
```

and consults it in `BlockBasedTableBuilder` for every data block that
`prepopulate_block_cache` would warm, with the block's first and last keys
(the builder already tracked the last key; the patch records the first). Meta
blocks are not filtered, and the parallel-compression path is not (its keys
are gone by insertion time), so with the filter unset behaviour is unchanged.

The adapter's side (`--warm_mode=prepop`): the ranges chosen at a job's
`OnFlushBegin`/`OnCompactionBegin` are handed to the builder through a
thread-local, since RocksDB runs those callbacks on the thread that then
builds the job's tables (with subcompactions off, the harness's default) and
the builder has no job id; the filter maps the block's key span to range ids
and answers yes iff it overlaps a chosen range and the job's block budget is
not spent. Nothing is read back at `End`. This is the same mechanism and the
same cost as `kFlushAndCompaction`, plus selection, and the comparison that
follows is the first apples-to-apples one on RocksDB.

Same three configurations as H17/H18 (RocksDB, 3 GB block cache; models from
the `_v5` runs):

| | stock | `kFlushOnly` | `kFlushAndCompaction` | Leaper, sst re-read (H) | **Leaper, prepopulate** |
|---|---|---|---|---|---|
| paper scale, lifecycle 60 s | 90.12% | +1.40pp | +1.28pp | +1.55pp | **+1.53pp** |
| IM shape on a 10 GB table | 54.54% | +1.88pp | +6.52pp | +8.71pp | **+8.56pp** |
| IM at its own 8m-row size | 70.18% | +8.21pp | **+19.13pp** | +15.39pp | +15.48pp |

Blocks warmed per run, prepopulate against sst: 248k / 250k, 1.17M / 1.18M,
2.11M / 2.10M — the filter admits the same blocks the re-read mode fetched,
now from memory, and rejects 3.1M, 3.2M and 0.4M others.

**The re-read never mattered.** Leaper through RocksDB's own warming path
lands within 0.2pp of Leaper through the re-read path on all three
configurations, so the asymmetry M8 flagged as its most worthwhile remaining
work explained none of the RocksDB margins. In particular the 8m-row case
still goes to warming everything by 3.7pp with both policies paying the same
zero I/O: that gap is selection — a model at 76% precision and recall 1.0
that admits three quarters of the blocks warming everything admits, on a
cache that would have held all of them — and not cost. And the IM-on-10 GB
case keeps its +2.0pp over `kFlushAndCompaction` with the cost advantage
removed, so that margin is selection too, in the other direction.

Two things the prepopulate path settles that the sst path could not. It
warms at the priority RocksDB chooses (LOW for flush outputs, BOTTOM for
compaction outputs — RocksDB 11.8's `kFlushAndCompaction` is itself a mild
form of selection), so Leaper's blocks compete for the cache on exactly the
built-in's terms. And it costs what the built-in costs: the adapter's whole
per-job overhead is the inference (0.8-5 us per range) and one range lookup
per data block.

## 2. The FAST'20 workload model

The journal version needs a workload that is not this repository's own
generator. The obvious candidate, Facebook's production RocksDB traces from
the FAST'20 characterisation (ZippyDB, UDB, UP2X), does not exist publicly:
the paper says so ("We are not releasing the trace at this time", Section 8),
SNIA IOTTA's key-value collection holds Twitter's and IBM's traces only, and
what Facebook released is the *model* it fitted to ZippyDB and shipped in
`db_bench` as `mixgraph`. That model is ported here as `--key_dist=mixgraph`
with the paper's Prefix_dist parameters: the key space is cut into 30 key
ranges whose access probability follows a two-term exponential
(a = 14.18, b = -2.917, c = 0.0164, d = -0.08082), the key inside a range
follows a power law (a' = 0.002312, b' = 0.3467) over a seed that is hashed
to an offset, and the query rate follows a sine wave (`--sine_a/b/d`) whose
period at the paper's parameters is 86,069 s, the diurnal cycle — flat to
0.5% over a 300 s run, and flat in every run here (the harness's first
scaling of it had a period of 0.086 s, which averaged out inside each
one-second row; the units now match `db_bench`'s). The port follows `db_bench`'s `GenerateTwoTermExpKeys` step
for step, with a different seed hash, so key identities differ from
`db_bench`'s and the distribution's shape does not.

`mixgraph_check` says what the model looks like at Leaper's granularity:

| | |
|---|---|
| hottest key range's share of accesses | 80.7% |
| next two | 5.7%, 1.5% |
| coldest | 0.15% |
| 2,000-key ranges touched per ZippyDB-second (4,900 reads) | **6.8%** |
| 10,000-key ranges | 17.9% |
| 100,000-key ranges | 67.7% |

So unlike the stationary zipfians of H18, this model leaves most of the key
space cold at any moment — the prediction label is informative, and there is
something for selection to select. What it does not have is any movement of
the hot set: the ranges' hotness is fixed for the run (the paper lists
"correlations between queries" as future work), so the learned model can do
no better than the naive rule "hot last interval, hot next" on it. It tests
the mechanism of selective warming on a third-party model of a production
workload, not the learning.

Run at the paper's own scale — 50M records, 43-byte values, a 256 MB block
cache (`cache_size=268435456` in the paper's command lines), 86/14
read/update with the 3% seeks folded into reads — but at 60,000 operations
per second rather than ZippyDB's 4,900, so that 300 s of run carry about an
hour of ZippyDB's writes; a 4 MB write buffer keeps flushes and compactions
frequent enough to matter in that window.

**RocksDB** (`m7zippy`; 647 MB of compaction and 104 MB of flushes in the
300 s window; model over 25,000 ranges: positive rate 0.293, precision 0.872,
recall 0.264, AUC 0.764 against the naive rule's 0.657):

| policy | hit ratio | vs stock | blocks warmed |
|---|---|---|---|
| stock | 81.15% | — | — |
| `kFlushOnly` | 81.31% | +0.16pp | |
| `kFlushAndCompaction` | **82.03%** | **+0.88pp** | all |
| Leaper, sst re-read | 81.94% | +0.79pp | 159k |
| Leaper, prepopulate | 82.00% | +0.85pp | 172k |

On the FAST'20 model every warming policy that touches compaction output is
worth about +0.9pp, and selection neither adds nor costs anything: Leaper
and `kFlushAndCompaction` are within RocksDB's noise of each other. The
regime map says why. The hot key range is about 100 MB of a 3 GB database
and the block cache is 256 MB, so the cache holds the working set with room
to spare; that is the corner in which recall beats precision, and here the
model's recall is 0.26 — inside the one hot range the keys are a hashed
power law, so which 2,000-key slice of it is read in a given second is close
to a coin toss, and the model can only be precise about the few slices that
are hot every second. What it declines to warm (it admits 172k blocks where
warming everything admits every output block) would have been read from a
cache that had space for it. The paper's own Table 2 puts ZippyDB's
counterpart, the e-commerce workload, at zipf 0.3 with a 6:1 read/write
ratio: the same read-heavy, cache-fits shape, and the same verdict as H18-D.

**LevelDB** (`m4zippy`, same model, same scale and rate): the engine is
outside its envelope here — 108 MB of user writes became **78 GB** of
compaction output in 300 s (2,149 compactions), a write amplification near
700 from a 10 MB L1 under a 3 GB database, and the single background thread
ran at capacity throughout.

| policy | hit ratio | vs LRU | QPS | compactions in window |
|---|---|---|---|---|
| LRU | 73.56% | — | 60,194 | 1,783 |
| EagerEvict | 74.26% | +0.70pp | 60,150 | 1,739 |
| IncrementalWarmup | 72.02% | -1.53pp | 60,253 | 1,628 |
| WarmAll | 65.38% | -8.18pp | 60,225 | 1,606 |
| WarmFlushOnly | 74.44% | +0.89pp | 60,147 | 1,743 |
| Leaper (prefetch only) | 84.61% | +11.05pp | **65,993** | **671** |
| Leaper, warm reads on a separate thread | 84.59% | +11.03pp | 65,765 | 664 |

**Where the +11pp comes from took four more runs to settle, and the first
answer written here was wrong.** The Leaper row is the only one whose QPS
exceeds the harness's rate budget and whose compaction count is a third of
everyone else's, and the run's own statistics said why the thread was busy:
44.0M inferences at 4.95 us each, **217.6 s of the 300 s run**, on LevelDB's
one background thread. The model predicts over 25,000 ranges, and every
flush and every L0 compaction has inputs spanning the whole key space, so
each of them asked for 25,000 ranges times k1 + k2 steps. The obvious
reading — 72% of the compaction thread spent in inference throttled
compaction, a third as much cache was invalidated, and the hit ratio rose
for a reason that has nothing to do with what was warmed — is what the
previous revision of this document and commit 9e2c816 said. The controls
say otherwise:

| control | what changed | hit ratio | vs LRU | QPS | compactions | inference |
|---|---|---|---|---|---|---|
| Leaper (prefetch only), as above | — | 84.61% | +11.05pp | 65,993 | 671 | 217.6 s |
| warm reads on a separate thread | `--warm_async` | 84.59% | +11.03pp | 65,765 | 664 | 210.7 s |
| C1: one prediction step | `--model_steps=1` | 84.88% | +11.32pp | 62,175 | 984 | 145.1 s |
| C2: 100,000-key ranges (500 of them), retrained | `RANGE_SIZE=100000` | 65.72% | -7.83pp | 60,228 | 1,587 | 0.7 s |
| **D: dry run — same inference, predictions discarded** | `--leaper_dry_run=1` | **74.53%** | **+0.97pp** | 65,578 | 668 | 218.9 s |

The dry run is the decisive one. It pays the same inference on the same
thread (44.3M inferences, 219 s), throttles compaction to the same degree
(668 against 671), and warms nothing — and it lands at +0.97pp, of which
+0.70pp is the dead-block floor every warming policy shares. **Throttling
compaction is worth about +0.3pp here; the other ten points of Leaper's
margin are the prefetching.** C1 agrees from the other side: a third less
inference and 47% more compactions leave the hit ratio where it was. C2 is
not a control at all, in hindsight: at 100,000-key ranges the positive rate
is 0.997, the model predicts everything hot, and Leaper becomes WarmAll
(-7.83pp against WarmAll's -8.18pp, 1,587 compactions against 1,606); it
removes the inference and the selection together.

So this is the paper's regime, on the paper's chosen workload model, on the
engine that rewrites 700 times its ingest: a 256 MB cache that holds the hot
range with room to spare, compaction that invalidates every hot block many
times a minute, and a selection that warms 2.7M blocks in the window, 66% of
them read at least once, where warming everything warms 15.7M at 8% and
thrashes the cache. Warming the 29% of ranges that will be read restores
what compaction destroyed; warming all of it evicts the working set to make
room. The margin over the best non-learned policy (WarmFlushOnly, +0.89pp)
is ten points, the largest measured anywhere in this repository, and the
first result here where the learned selection beats every heuristic by far
more than the noise floor on a workload model that is not this repository's
own.

**The QPS excess was the harness, not the engine.** Every Leaper and dry-run
row runs at 65-66k operations per second against a 60k budget, and the rate
limiter cannot leak: it is one global counter against `op_rate x elapsed`.
What drifted was the monitor thread, whose per-second loop was "take the
stats, then sleep one second"; taking the stats means `Leaper::stats()`,
which takes the core's mutex, which a prediction holds for up to 250 ms at
a time, 72% of the time. Each row stretched by the wait, 300 rows spanned
about 325 s of wall clock, and the QPS column read the extra 25 s of budget
as a 10% overshoot (LevelDB's own LOG clock confirms it: the Leaper run's
330 monitor iterations took 351 s, stock's 324 s). That is the seventeenth
defect. Hit ratios are ratios and are unaffected; the compaction column for
Leaper rows counts a ~325 s window, which understates the throttling
slightly (671 in 325 s is about 620 in 300); the RocksDB harness did not
drift (all `m7zippy` rows within 0.1% of 60k) because its monitor reads the
core's statistics only at the end of the run. Both monitors now sleep to
the tick rather than for a second.

**And the inference cost is real, so it is now removed rather than
excused.** Every feature of a range — the completed slots' rates, the
precursors' rates, the hour, minute and second — is constant within a
wall-clock second, so the ten jobs LevelDB starts in a typical second on
this model were making the same 25,000 predictions ten times over. The core
now memoises predictions per (second, slot, step range)
(`Options::memoize_predictions`, on by default, pinned by `core_check`), and
the same configuration re-measured with the memo and the tick-aligned
monitor gives:

| policy, v9 harness (tick-aligned monitor, memoised predictions) | hit ratio | vs LRU | QPS | compactions | inference | memo hits |
|---|---|---|---|---|---|---|
| LRU (stock) | 73.53% | — | 59,998 | 1,756 | — | — |
| **Leaper (prefetch only)** | **84.86%** | **+11.32pp** | 59,999 | 1,066 | 109.7 s (21.8M inferences) | 19.3M |

Stock reproduces to 0.03pp, and both rows now sit on the 60k budget to the
operation (each window's operations sum to exactly 300 s of budget: the
drift is gone). The memo answers 47% of prediction requests — not the 90%
the job rate suggested, because k1 varies with each compaction's size and
a job with a different step range is a different memo — so inference falls
from 218 s to 110 s, compactions recover from 671 to 1,066 against stock's
1,756, and the hit ratio does not move: +11.32pp, with 3.0M blocks warmed
in the window at a measured prefetch precision of 0.57. The dry run said
the throttling was worth a third of a point; halving the throttling moved
the result by a quarter of one, in the direction the dry run predicts. The
remaining third of the thread is the model's own cost at this granularity.
Taking it off the compaction thread entirely, or bounding the candidates to
what a job's output can contain, is the design work left — and it is now
separable from the result.

Two lessons for the write-up. A policy's cost on the background thread is a
confound on a compaction-bound engine, and the compaction count and the QPS
column are what expose it; but the way to size the confound is a dry run
that pays the cost and discards the answer, not a coarser model that removes
the cost and the selection together. And the paper's Table 5 inference
figure (1-5 ms per compaction) assumes a few hundred candidate ranges; at
25,000 it is 300 ms per job, which on an engine whose flushes span the whole
key space is one second's worth of prediction repeated for every job in that
second, and memoising it is the fix.

## 3. Where this leaves the claims

Of the five items listed as necessary before a journal write-up, four are
done (multi-seed variance is section 4, the phase-1 decision section 5)
and one changed shape on the way.

**Selective prepopulate (item 3) is settled and closes M8's largest open
question.** Leaper through RocksDB's own warming path is within 0.2pp of
Leaper through the re-read path on all three configurations, so none of the
RocksDB margins reported in section H were cost artefacts: the +2.0pp over
`kFlushAndCompaction` on the IM shape over a 10 GB table is selection, and
the -3.7pp under it at the 8m-row size is selection too. The patch is 43
lines and changes nothing when the filter is unset; the adapter's per-job
overhead is now the inference plus one range lookup per data block, which
is the paper's own cost model.

**A workload that is not this repository's generator (item 1) exists, with a
caveat that has to travel with it.** Facebook's ZippyDB model is the closest
public thing to the traces a referee would ask for, and it is a model: 30
key ranges of fixed hotness, a power law inside each, no movement of the hot
set over time. It tests selective warming, not learning — the learned
model's advantage over "hot last interval, hot next" is an AUC of 0.76
against 0.66 on it. On that model the two engines give the two ends of the
regime map in one experiment:

| engine | write amplification in the window | best heuristic | Leaper (prefetch phase) |
|---|---|---|---|
| RocksDB 11.8, 3 GB cache | ~6 (647 MB of compaction on 104 MB flushed) | `kFlushAndCompaction` +0.88pp | +0.79 / +0.85pp: within noise of it |
| LevelDB 1.23, 256 MB cache | ~700 (78 GB on 108 MB) | WarmFlushOnly +0.89pp | +11.05pp throttled, +11.32pp with the memo: ten points clear |

RocksDB's compaction destroys so little that there is nothing to recover and
warming everything is free; LevelDB's destroys so much that warming
everything thrashes a cache the hot set fits in, and choosing the 29% that
will be read is the whole game. Neither engine is X-Engine. The honest
sentence for the journal is that the value of learned selection is a
function of write amplification times cache pressure, that this repository
can place both public engines on that curve, and that where the paper's
engine sits on it is a number the paper's own data has to supply next to
every result.

**What the LevelDB result cost to believe.** The +11pp survived an
async-warm control, a one-step control and a dry-run control, and the last
of those is the one that should have been run first: it is the only control
that removes the answer and keeps the cost. The earlier revision of this
document called the result an artefact on the strength of the compaction
count alone, and commit 9e2c816 carries that claim in its title; the dry
run retracts it. The harness defect the investigation turned up (the
monitor's clock drift, the seventeenth) inflated a QPS column, not a hit
ratio, but it is exactly the kind of thing a referee finds first.

**Still open.** A dedicated machine (item 2): the overhead and latency
columns here were measured on a laptop behind a load gate, and the
tail-latency non-repeatability noted in M8 has not been revisited.
Multi-seed variance (item 4) is section 4: it settles every ordering in
this document except one, the ordering among the three warming policies at
the paper's scale on RocksDB, which it shows to be inside the seed spread.
Phase 1 (item 5) is section 5: given compactions of 2, 6 and 28 s it is
inert at every length, for a reason the cache occupancy makes plain, and
the journal version presents Leaper as a prefetcher.

## 4. Multi-seed variance

Four more evaluation seeds (1235-1238) for the six cells the claims rest
on, models fixed (trained on seed 42), every policy in a cell on one
binary (LevelDB `bench_leveldb_v10`, RocksDB `bench_rocksdb_v8`: the memo,
the tick-aligned monitor and the sine units all in). Margins are paired per
seed — the same seed is the same traffic for every policy — and reported as
mean ± sample sd over the four seeds, with the range; `tools/seed_stats.py`
computes them and section M9.3 of `experiments/results/M8-report.txt` has
the full tables. The seed-1234 rows of sections 1-2 and of M8 were measured
on earlier binaries and are quoted as a fifth point only where that does
not matter. 84 runs, 7 h 07 min behind the quiet-machine gate.

| cell | stock, mean ± sd | policy | vs stock, mean ± sd (range) | paired vs the built-in |
|---|---|---|---|---|
| LevelDB, ZippyDB model, 256 MB | 73.57 ± 0.01 | WarmFlushOnly | +0.92 ± 0.10 (+0.85..+1.07) | |
| | | WarmAll | **-1.10 ± 4.01 (-3.51..+4.89), mixed sign** | |
| | | Leaper (prefetch) | **+11.24 ± 0.08 (+11.19..+11.35)** | +10.32 ± 0.04 over WarmFlushOnly |
| RocksDB, paper scale, lifecycle 60 s | 90.53 ± 0.24 | `kFlushOnly` | +1.38 ± 0.21 (+1.14..+1.58) | |
| | | `kFlushAndCompaction` | +1.39 ± 0.32 (+0.95..+1.69) | |
| | | Leaper (prepopulate) | +1.65 ± 0.09 (+1.53..+1.72) | +0.26 ± 0.30 over `kFlushAndCompaction` (min +0.02); +0.27 ± 0.25 over `kFlushOnly`, one seed negative |
| RocksDB, IM shape on 10 GB | 54.61 ± 0.02 | `kFlushAndCompaction` | +6.53 ± 0.03 | |
| | | Leaper (prepopulate) | **+8.52 ± 0.03** | +1.99 ± 0.01 over `kFlushAndCompaction` |
| RocksDB, IM at 8m rows | 70.19 ± 0.02 | `kFlushAndCompaction` | **+19.21 ± 0.06** | |
| | | Leaper (prepopulate) | +15.55 ± 0.05 | -3.67 ± 0.08 under `kFlushAndCompaction` |
| RocksDB, ZippyDB model, 256 MB | 81.14 ± 0.02 | `kFlushAndCompaction` | +0.89 ± 0.01 | |
| | | Leaper (prepopulate) | +0.87 ± 0.01 | -0.02 ± 0.01: equal |
| LevelDB, NVMe, 128 MB (H2) | 83.30 ± 0.33 | EagerEvict | +0.19 ± 0.03 | |
| | | WarmAll | -0.02 ± 0.13, mixed sign | |
| | | Leaper (prefetch) | **+2.99 ± 0.12 (+2.84..+3.11)** | +2.80 over the floor |

What the seeds change, and what they do not.

**Every Leaper margin over stock keeps its sign on every seed, and every
one of them is tighter than the corresponding built-in's.** On the two
LevelDB cells the stock hit ratio itself moves with the seed (83.00-83.75%
on H2) and the paired margins do not (+2.84..+3.11); on RocksDB at the
paper's scale the built-ins' margins spread 0.4-0.7pp across seeds where
Leaper's spreads 0.2.

**The ordering among the three warming policies at the paper's scale on
RocksDB is not established.** Leaper is above `kFlushAndCompaction` on all
four seeds, by +0.02 to +0.70, and above `kFlushOnly` on three of four;
section H's "+1.55 against +1.30 and +1.39" was one seed of a spread the
size of the differences. The honest statement is that the three are within
a third of a point of each other there, with Leaper's margin over stock the
most repeatable of the three. The two IM cells and the two ZippyDB cells
are the opposite: seed spreads of 0.01-0.08pp against margins of 2.0, 3.7,
0.0 and 10.3 points, so those orderings are settled.

**WarmAll on the LevelDB ZippyDB cell is not a number; it is a function of
how much compaction the run happened to do.** Its four seeds gave 70.09,
70.49, 70.84 and 78.46%, and the seed-1234 run 65.38%; the compaction
volume of those five runs was 47.8, 47.0, 46.0, 26.7 and 62.2 GB — the
ordering is exact. Warming every output block makes the cache's contents a
function of the compaction stream: more compaction, more thrash. Leaper's
runs compacted between 22.8 and 42.3 GB across the same seeds and its hit
ratio stayed within 84.76-84.92%; WarmFlushOnly's stayed within
74.44-74.64% over 25.4-49.5 GB. Selection decouples the cache from the
compaction volume; warming everything couples it. That is section 2's dry
run seen from the other side.

**LevelDB's compaction volume is not a property of the workload alone.**
Across seeds, stock's compaction in the window ranges from 46.6 to 70.4 GB
for the same writes. And on seed 1238 the stock run compacted 52.6 GB while
every policy with a warm path on the compaction thread compacted about half
of it — WarmFlushOnly 25.4, WarmAll 26.7, Leaper 22.8 GB — with the same 55
flushes, no trivial moves, and 8% more SST files alive at the peak: the
compaction did not disappear, it lagged, and the sequence of jobs LevelDB
picked from that different level-0 state rewrote half as much. Counts and
bytes move together throughout (30-38 MB per job), so the compaction count
column is a fair proxy for volume; but neither is a fair proxy for "the
workload", because on this engine how much compaction happens is partly
the policy's doing. One more reason the dry run, not the compaction count,
is the control.

## 5. The phase-1 decision

Phase 1 is the eviction half of the two-phase prefetcher: at compaction
begin, input blocks predicted cold for the compaction's duration T1 are
dropped from the cache, so that the space serves reads during T1 instead
of holding blocks the compaction will invalidate anyway. Every earlier
LevelDB comparison put it within 0.1pp of the prefetch phase alone (+0.04
on NVMe, +0.11 on slow storage with a 64 MB cache; M8, section I), and
RocksDB cannot implement it as a plug-in. But those compactions ran for
0.12-2 s, and a phase whose gain is proportional to T1 had never been
given a T1. This sweep gives it one: the H14 configuration (slow storage,
128 MB cache, 40 s hot lifetimes — the cell where selection pays most on
LevelDB), with the SST size raised 4 -> 16 -> 64 MB so that a compaction
runs 2 -> 6 -> 28 s at the same compaction volume (1.9-2.2 GB per 180 s
window: the same writes, rewritten in fewer, larger jobs), 24 prediction
steps so that k1 + k2 still fit, and the eviction phase run on its own
(`leaper_p1only`) as well as with and without the prefetch phase. One
seed; the slow-storage noise floor is 0.3pp. A single >50% CPU process was
present on the machine for eleven minutes across the 16 MB matrix and the
64 MB training run, with the load never above 2.7 on ten cores.

| SST size | T1, median | compactions in window | stock | EagerEvict (floor) | phase 1 alone | phase 2 alone | both phases | phase 1 on top of phase 2 |
|---|---|---|---|---|---|---|---|---|
| 4 MB | 2.1 s | 56 | 78.53% | +1.56 | +1.52 | +7.93 | +7.68 | -0.25 |
| 16 MB | 5.6 s | 18 | 79.58% | +1.19 | +1.01 | +2.59 | +2.88 | +0.29 |
| 64 MB | 28 s | 4 | 81.52% | -0.20 | -0.36 | -0.19 * | -0.33 * | -0.14 |

\* At 64 MB the prefetch phase asked for steps 29-38 and the run had 24
models: five of its six compactions were clamped (the new `clamped`
counter) and it warmed 3.4k blocks instead of the ~100k of the other
sizes. Those two cells measure phase 1 plus nothing, not prefetching.

**Phase 1 is worth nothing at any compaction length reached here.** On its
own it sits on the floor (-0.04, -0.18, -0.16 against EagerEvict); on top
of the prefetch phase it is -0.25, +0.29 and -0.14, all inside the noise
floor and not monotone in T1. Not for want of trying: the phase evicted
370k, 435k and 461k blocks early per run (the evict-only run's `evicted`
count less the floor's), with 80% of the candidate ranges predicted cold
each time.

**Why: the resource it frees is not scarce in the regime where it acts.**
Under stock LRU the cache is full, 128.0 MB every second. Under any policy
that reclaims dead blocks it is not: 112 MB on average with 4 MB files,
104-106 MB with 16 MB files, and with 64 MB files it falls to 7-14 MB at
the end of each compaction — a 64 MB-file compaction invalidates nearly
the whole cache at once — and refills over the ~10 s of T2. Phase 1 evicts
into a cache that already has 10-20% free on average, and that is nearly
empty at exactly the moment the compaction it was evicting for finishes;
the reads during T1 were never short of space. The regime the paper
designed phase 1 for is the opposite one: a cache that stays full through
a long compaction whose inputs are a large share of it, so that holding
predicted-cold input blocks for minutes costs hits. That needs long
compactions *and* a full cache *and* high reuse, and on these two engines
the three do not coincide: long compactions here come from big files that
also wipe the cache, and the cache-pressure regimes (cache below the
working set, M8 section H) are the ones where no warming policy pays at
all — phase 1 measured +0.11 there.

**Decision.** The journal version presents Leaper as a prefetcher. Phase 2
is the mechanism behind every margin in this repository; phase 1 is an
option for engines whose compactions are long, parallel and small relative
to the cache — X-Engine's, by the paper's description — with the sweep
above as the evidence that it is inert on LevelDB and not implementable
on RocksDB. The code keeps both phases (`Options::enable_phase1`, default
on, for fidelity to the paper); the headline rows are the prefetch phase
alone, as they have been since M8.

Two side findings belong in the write-up. At a fixed compaction volume,
bigger SSTs raise the stock hit ratio and shrink every warming margin
(stock 78.5 -> 79.6 -> 81.5%; Leaper +7.9 -> +2.6 -> nothing measurable),
because the same bytes are invalidated in fewer, larger events with time
to refill between them: the size of the invalidation problem is not the
compaction volume alone but its granularity. And a fixed horizon of 24
one-second steps is too short for a 28 s compaction; an engine with long
compactions needs a coarser statistical interval rather than more models,
since on 40 s lifetimes the models past step 12 are already at precision
0.5 and past step 20 at 0.2. Twenty-four steps in place of six cost the
4 MB prefetch row 0.2pp against H14 (7.93 against 8.13): with T2 = 16 s the
prefetch phase is a union over sixteen models, and the weak far ones admit
a little noise.
