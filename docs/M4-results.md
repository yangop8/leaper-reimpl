# M4 — Baseline matrix

> **Scope note.** These measurements come from a clean-room reimplementation on
> LevelDB/RocksDB with synthetic workloads and NVMe (slow storage is emulated).
> The paper's results come from X-Engine, real Tmall and DingTalk traffic, and
> spinning disks. The numbers below describe *this* setup and are not a
> replication of the paper's. See the top of the repository README.

## Protocol

`experiments/run_m4.sh` runs the whole thing in one command:

1. **Train** — seed 42, `policy=off`, access trace captured.
2. **Fit** — models for steps 1..6, precursors, and the phase constants
   `alpha`/`beta` calibrated from that same run's compaction log.
3. **Oracle** — a second `policy=off` run on **seed 1234** whose trace becomes
   the oracle's per-interval hot sets.
4. **Evaluate** — every policy on seed 1234, identical in every other respect.

Training and evaluation use different seeds, so the model never sees the
workload realisation it is scored on. The oracle is built from the evaluation
trace on purpose: it is the upper bound and is meant to know the future.

Workload: 4M keys (~470 MB), 128 MB block cache (27%), lifecycle generator with
16 chains of 4 ranges, 8 s lifetimes, 40k ops/s of which 4k are writes, 300 s
measured after 30 s warmup, NVMe with the OS page cache bypassed.

## Two bugs found by this matrix, both invisible to smoke tests

The first matrix put **Leaper below plain LRU** (-0.52pp). Neither cause was a
tuning issue.

**1. Index keys are physically shortened.** `InternalKeyComparator::FindShortestSeparator`
(`db/dbformat.cc:72-77`) truncates the user key and increments its last byte, so
the 16-digit key `0000000000123456` reaches the index as the 14-byte string
`00000000001235`. Parsing that as an integer gives 1235, not 123500 — wrong by
a factor of 100 and systematically biased toward range 0. Every block bound read
from an index was affected, which corrupted both the candidate set and the
blocks phase 1 chose to evict. Fixed by restoring the key to its full width
before dividing, which is exact rather than approximate: the separator S
satisfies `last_key <= S < next_first_key` as byte strings, and padding with the
smallest digit preserves both inequalities against fixed-width keys.

**2. The last index entry of every SST is one byte long.**
`BytewiseComparatorImpl::FindShortSuccessor` (`util/comparator.cc:54-64`)
increments the first byte that is not 0xff and truncates there, so
`0000000000123456` becomes the single character `"1"`. Restored to full width
that is 10^15 — range id **25 billion**. Expanding that span into individual
range ids allocated until the OS killed the process, which is exactly what
happened to the `incremental_warmup` run. Fixed with `Options::max_range_id`
plus a hard cap in the expansion itself.

`leaper/src/mapper_check.cc` is the regression test for both, including an
explicit assertion that the short successor really does map out of the key space
so that callers must clamp.

## The two phases pull in opposite directions

Once the mapping was correct, Leaper was still below LRU. Disabling each phase
separately (60 s runs, same workload) shows why:

| configuration | block cache hit ratio | vs LRU |
|---|---|---|
| LRU (stock) | 71.33% | — |
| Leaper, both phases | 70.61% | **-0.72pp** |
| Leaper, phase 1 only (evict) | 71.00% | -0.33pp |
| Leaper, phase 2 only (prefetch) | **73.31%** | **+1.98pp** |

**The eviction phase costs about 2.7 percentage points.** The mechanism is not
mysterious. Phase 1 evicts input blocks predicted cold for the duration `T1` of
the compaction, and the step-1 model's recall is 0.807 — so roughly a fifth of
the ranges that *will* be read are predicted cold, and their blocks are dropped
while the input files are still live and still serving reads. Each of those is a
guaranteed miss.

What it buys in return is small *on LevelDB specifically*, because the blocks it
frees early are about to become garbage anyway when the compaction installs its
output, and **EagerEvict already reclaims them at that moment with no risk at
all**. Phase 1 moves the same reclamation earlier, and pays for the head start
with the model's false negatives.

This is a property of the setting, not a refutation of the paper. X-Engine has a
row cache as well as a block cache, much longer compactions (so `T1` spans many
intervals and the prediction is over a longer, more skewed horizon), and a cache
under real pressure where freeing space early has value. On LevelDB with a cache
that is not the binding constraint, the trade is negative. The matrix therefore
reports Leaper twice — with and without phase 1 — because reporting only their
sum would hide a result that points in opposite directions.

## Storage regimes

The paper's headline includes eliminating 99% of latency spikes. That
amplification comes from spinning disks, where a block cache miss costs
milliseconds; on the NVMe used here a miss costs tens of microseconds and the
tail barely moves (M0 measured `corr(compactions, read_p99) = -0.218`). The
matrix is therefore run in more than one regime, via `--read_delay_us`, which
adds a fixed delay to every `RandomAccessFile::Read` including compaction reads:

| regime | `--read_delay_us` | what it stands for |
|---|---|---|
| NVMe | 0 | the machine this was measured on |
| SATA / network block store | 200 | a cache miss costs ~10x more |
| 7.2k RPM disk | 5000 | the paper's regime |

Prefetching is worth more the more a miss costs, so the NVMe numbers are the
*pessimistic* end for every warming policy, not the representative one.

## Two experiment-design errors worth recording

Both produced plausible-looking tables that would have been wrong.

**Every policy must start from the same database.** The workload inserts new
keys (5% of operations, ~260k per run). Running seven policies back to back
against one database grew it by 46%, monotonically, so the last policy was
scored on a database half again as large as the first. Fixed by rebuilding the
database before every evaluation run.

**A shared prefetch budget changes what is being compared.** An early matrix
capped every policy at 10% of the cache per compaction. Under that cap
"WarmAll" is not warm-all, it is warm-the-first-10%, and the cap — not the
policy — was doing the work: the oracle scored *below* WarmAll (+2.08pp vs
+2.26pp), which is impossible if selection has any value and the budget is not
binding. The cap is now off by default, and any budget used is reported.

## Results (NVMe, page cache bypassed)

Every policy from an identical database, no prefetch budget, 300 s measured.

| policy | hit ratio | vs LRU | QPS | p95 | p99 | misses |
|---|---|---|---|---|---|---|
| LRU (stock) | 72.66% | — | 40,181 | 12 us | 22 us | 3,551,269 |
| EagerEvict | 74.54% | +1.88pp | 40,100 | 14 us | 26 us | 3,304,431 |
| IncrementalWarmup | 68.28% | **-4.39pp** | 40,100 | 17 us | 34 us | 4,125,050 |
| WarmAll | **74.80%** | **+2.14pp** | 40,169 | 13 us | 25 us | 3,280,670 |
| Leaper (both phases) | 72.09% | -0.58pp | 40,110 | 12 us | 23 us | 3,620,891 |
| Leaper (prefetch only) | 74.55% | +1.88pp | 40,167 | 14 us | 26 us | 3,308,010 |
| Oracle (1-interval lookahead) | 74.66% | +1.99pp | 40,201 | 15 us | 29 us | 3,294,392 |

### What this actually says

Every policy except `off` also reclaims the block cache entries of SSTs that
compaction deleted, because that is free and safe. So the rows decompose:

| contribution | value |
|---|---|
| reclaiming dead blocks (EagerEvict) | **+1.88pp** |
| warming everything on top of that (WarmAll) | +0.26pp |
| warming with perfect 1-interval knowledge (Oracle) | +0.12pp |
| warming with the learned model (Leaper) | **+0.01pp** |
| Leaper's eviction phase | **-2.46pp** |

**Nearly all of the apparent win is not learned.** It comes from reclaiming
blocks LevelDB leaks — a bug-fix-shaped change with no model in it. The entire
prefetch mechanism is worth at most 0.26pp here even when it is told exactly
what will be read next, and the learned version captures essentially none of
that. This is the single most important number in the reproduction, and it
would have been invisible without EagerEvict as a separate baseline.

**The oracle scoring below WarmAll is not a contradiction.** The oracle
implemented here has one interval of lookahead, matching the paper's prediction
target. WarmAll has unbounded lookahead at the cost of warming everything, so on
a workload where the cache is not scarce it can beat a 1-interval oracle. That
ordering is itself the finding: **selection has no value when warming costs
nothing**, and warming costs nothing here because the block cache is not the
binding constraint and a miss on NVMe is tens of microseconds.

**IncrementalWarmup is worse than doing nothing** (-4.39pp), which matches the
paper's own assessment of it as a weak baseline, and reproduces for the reason
the paper gives: it assumes newly compacted blocks are hot if they overlap
anything currently cached, which on a skewed workload warms a great deal of
cold data while evicting the cached blocks it overlapped.

### The regime this does not cover

These numbers are the pessimistic end for every warming policy. Prefetching pays
in proportion to what a miss costs, and on this hardware a miss costs ~20 us.
The paper's setting is spinning disks, where a miss costs milliseconds and the
hit-ratio recovery after a compaction is long. Reproducing the paper's *relative*
gains requires the slow-storage regime (`--read_delay_us`), and a conclusion
about whether Leaper's prefetch is worth its complexity cannot be drawn from
NVMe numbers alone.

## Results (emulated slow storage: `--read_delay_us=200`, 64 MB cache, 8k ops/s, 180 s)

This is the regime the paper lives in: a block cache miss costs ~200 us instead
of ~20 us, and read p95 rises from 12 us to 271 us, so the latency amplifier
that M0 found missing on NVMe is back.

| policy | hit ratio | vs LRU | p95 | p99 | misses |
|---|---|---|---|---|---|
| LRU (stock) | 35.83% | — | 271 us | 286 us | 1,101,174 |
| EagerEvict | 37.36% | +1.53pp | 350 us | 405 us | 1,078,255 |
| IncrementalWarmup | 37.21% | +1.38pp | 272 us | 289 us | 979,592 |
| WarmAll | **41.09%** | **+5.26pp** | 294 us | 310 us | 885,904 |
| Leaper (both phases) | 37.19% | +1.36pp | 289 us | 304 us | 1,077,148 |
| Leaper (prefetch only) | 37.28% | +1.44pp | 271 us | 286 us | 1,073,340 |
| Oracle (1-interval) | 38.74% | +2.91pp | 313 us | 344 us | 1,031,114 |

Warming is worth much more here (+5.26pp against +2.14pp on NVMe), exactly as
expected once a miss is expensive. But the ordering is unchanged and the gap
between the oracle and WarmAll is now *larger*, which sharpens the diagnosis
rather than reversing it.

### The prediction target is the limiting factor, not the model

Relaxing Leaper toward "warm more" improves it monotonically, all the way to
WarmAll:

| configuration | hit ratio |
|---|---|
| Leaper, calibrated T2 (~2 s), threshold 0.5 | 37.28% |
| Leaper, T2 pinned to 6 s (all six multi-step models unioned) | 37.42% |
| Leaper, T2 = 6 s and threshold lowered to 0.2 | 38.96% |
| Oracle, 1-interval lookahead | 38.74% |
| WarmAll | 41.09% |

Every increment of selectivity costs hit ratio. That is not a statement about
this particular model — a perfect 1-interval oracle sits in the same band. It is
a statement about the **target**: "will this range be read in the next
interval" captures only part of why a block is worth warming. A block earns its
place in the cache because it will be read at some point over the next many
intervals, and the paper's formulation, faithfully implemented, cannot express
that.

The reason selectivity does not pay *here* is measurable: in this workload 32-64
of 100 key ranges are hot at any moment, so warming everything a compaction
writes is already 30-60% precise, and the cache is not scarce enough for the
remaining waste to cost anything. Selectivity should pay when the hot fraction
is small — which is the regime a production e-commerce workload occupies and the
one the sparse-hot-set experiment below tests.

## Sparse hot set: the hypothesis that selectivity would pay, tested and refuted

The obvious explanation for WarmAll dominating was that our hot set is too dense
— 32-64 of 100 key ranges — so warming everything is already mostly right. If
so, shrinking the hot fraction should reverse the ordering. It does not.

Same slow-storage regime, but 1000 key ranges instead of 100, so roughly **3%**
of ranges are hot at any moment and warming everything wastes ~97% of its
inserts:

| policy | hit ratio | vs LRU | misses |
|---|---|---|---|
| LRU (stock) | 63.18% | — | 624,201 |
| **WarmAll** | **71.83%** | **+8.65pp** | 423,844 |
| Leaper (prefetch only) | 65.59% | +2.41pp | 585,093 |
| Oracle (1-interval) | 66.83% | +3.65pp | 557,006 |

WarmAll's lead over the oracle *grows* (8.65 vs 3.65 percentage points). The
hypothesis was wrong, and the reason is worth stating because it is the crux of
the whole reproduction:

**Warming a block that will not be read is nearly free, because what it
displaces is also unlikely to be read.** Under LRU the block cache fills with
whatever was missed most recently — the long tail of a skewed workload. A
freshly compacted block is, on average, a *better* bet than the tail block it
evicts, even when the prediction says it is cold. Selectivity only pays if
admitting a cold block evicts a hot one, and that requires the cache to be small
relative to the hot set rather than to the database.

## Conclusion, across four regimes

| regime | LRU | best selective policy | WarmAll |
|---|---|---|---|
| NVMe, dense hot set | 72.66% | 74.66% (oracle) | **74.80%** |
| Slow storage, dense hot set | 35.83% | 38.96% (Leaper, relaxed) | **41.09%** |
| Slow storage, sparse hot set | 63.18% | 66.83% (oracle) | **71.83%** |
| RocksDB, NVMe | 81.42% | 81.42% (Leaper) | 81.34% (`kFlushAndCompaction`) |

The result reproduces in one direction consistently: **on every workload tested,
warming every block a compaction writes beats every selective policy, including
an oracle that knows which ranges will be read next.** The learned prefetcher
works — it trains, it infers in ~2 us, its scorer matches LightGBM to 3e-8, its
precursors recover a planted relation at 11x chance — and none of that converts
into a hit ratio advantage on these workloads.

What this does and does not say:

* It does **not** refute the paper. X-Engine has a row cache as well as a block
  cache, much longer compactions, a far larger cache under real pressure, and
  real e-commerce and instant-messaging workloads. Every one of those changes
  the economics of a wasted insert, which is the quantity that decides whether
  selectivity pays.
* It **does** say that a reproduction on synthetic workloads and commodity
  hardware cannot demonstrate the paper's gains, and that any claim it can
  should be checked against a WarmAll baseline. That baseline is trivial to
  implement and RocksDB ships it; a learned prefetcher has to beat it, not LRU.
* The highest-value next step is unchanged from M1: **drive the pipeline with a
  real trace** (Meta's FAST'20 RocksDB traces, Twitter's twemcache traces). Every
  negative result here traces back to a property of the synthetic workload —
  unpredictable births, a hot set that is either too dense or too cheap to
  mispredict. Only a real access stream can settle whether that is the workload
  or the method.

## Why the cache/data ratio matters, and a second suspect

A reasonable objection to everything above: if warming everything is optimal,
the cache must be too rich, because otherwise the right design would be an
in-memory database with cold data compressed back to disk. Production caches are
a few percent of the data, not a quarter of it.

Two things are worth separating here.

**The paper's own ratio was richer than ours.** The paper's synthetic setup is
10 GB of data with a 3 GB block cache — 30%. The measurements above used 27% and
13.6%. So cache richness alone does not explain the difference between our
result and theirs.

**Relative to the paper's own baselines, the reproduction succeeds.** The paper
compares against LRU and Incremental Warmup, and against those Leaper wins here
too: 74.55% versus 68.28% for Incremental Warmup, a 6.3-point gap in the same
direction and of a similar magnitude to what the paper reports. The conclusion
only reverses against a baseline the paper does not consider — warming every
block a compaction writes. That baseline is worth taking seriously precisely
because RocksDB now ships it.

**The more likely culprit is a flaw in our workload, not the cache size.** In
the lifecycle generator, reads and writes are drawn from the *same* hot set, so
every block a compaction rewrites is by construction read-hot. That hands
"warm everything" a perfect prediction for free, and leaves selection with
nothing to select. Real workloads decorrelate the two — a catalogue is read
while an order log is written — and the entire value of choosing what to warm
lives in that gap. `--write_corr` controls it, and both sweeps below vary one
suspect at a time: the cache/data ratio at fixed correlation, and the
correlation at fixed ratio.

## Cache/data ratio sweep

20M keys (~2.3 GB), emulated 200 us device, block cache shrunk from 10% of the
data to 1%. Models and oracle from one training run, since both depend on the
workload and not on the cache.

| cache/data | LRU | WarmAll | Leaper (prefetch only) | Oracle |
|---|---|---|---|---|
| 10% (230 MB) | 22.33% | **26.68%** (+4.35pp) | 23.50% (+1.18pp) | 24.58% (+2.25pp) |
| 3% (70 MB) | 13.06% | **15.75%** (+2.69pp) | 13.45% (+0.39pp) | 14.37% (+1.31pp) |
| 1% (23 MB) | 8.30% | **9.33%** (+1.03pp) | 8.43% (+0.13pp) | 8.75% (+0.45pp) |

**A scarcer cache does not make selection valuable; it makes prefetching as a
whole less valuable.** Every policy's gain shrinks as the cache shrinks —
WarmAll from +4.35pp to +1.03pp — and the ordering never changes. With a 1%
cache a prefetched block is evicted long before anything reads it, so precision
has nothing to buy: what is warmed does not survive either way.

This also reframes why WarmAll wins, because "the cache is too rich" is not the
explanation: 23 MB of cache over 2.3 GB of data is not rich by any standard, and
WarmAll still wins there. **WarmAll is a recency-of-write prior, and LRU is a
recency-of-read prior.** On a workload where reads and writes touch the same key
ranges, the block a compaction just wrote is a better bet than whatever LRU is
currently holding, whatever the cache size. That is a statement about the
workload's read/write correlation, not about cache capacity — which is what the
correlation sweep isolates.

## Read/write correlation sweep

Same 2.3 GB database, 230 MB cache, 200 us device; `--write_corr` draws each
write from the read hot set with that probability and from an independent hot
set otherwise. At 0.2, four writes in five land on ranges nothing reads, so most
of what compaction rewrites is read-cold.

| write_corr | LRU | WarmAll | Leaper (prefetch only) | Oracle |
|---|---|---|---|---|
| 1.0 | 22.33% | **26.68%** (+4.35pp) | 23.50% (+1.18pp) | 24.58% (+2.25pp) |
| 0.5 | 21.94% | **26.63%** (+4.69pp) | 23.70% (+1.76pp) | 24.03% (+2.09pp) |
| 0.2 | 22.21% | **26.07%** (+3.86pp) | 23.83% (+1.62pp) | 24.53% (+2.32pp) |

Flat. **Decorrelating reads from writes does not help selection either.** The
hypothesis that WarmAll was being handed a free perfect prediction is wrong: even
when 80% of what it warms is read-cold, it still wins by the same margin.

## What actually makes warming free

Both intuitive explanations — the cache is too rich, and reads and writes share
a hot set — are refuted by measurement. The mechanism that survives is a
property of LRU:

**An inserted block lands at the MRU end and evicts from the LRU end, which on a
skewed workload holds one-hit tail blocks. Warming junk evicts junk.** Precision
buys nothing because the thing precision would protect — a cache entry that
would otherwise have been read — is not what gets displaced.

That gives a falsifiable condition for when selection *must* start to pay. Let
`T_warm` be the time for warmed data to turn the cache over, and `T_reref` the
interval at which a hot block is re-read. Warming junk is free while
`T_warm > T_reref` (hot blocks get refreshed to MRU before the junk reaches
them) and costly once `T_warm < T_reref`. At the settings above the two are
nearly equal — 2.4 GB compacted per 150 s turns a 230 MB cache over every ~14 s,
while a given hot block is re-read about every ~16 s — which is exactly why
warming is marginally free rather than clearly harmful.

Raising the write rate collapses `T_warm` without touching `T_reref`, and is the
regime a write-heavy production system occupies. That is the experiment that
should separate the policies, and it is the one worth running before concluding
anything about the method.

## Write-heavy mix

Mix changed from 75/20/5 (read/update/insert) to 30/65/5 at the same 6k ops/s,
so writes go from ~1.5k/s to ~4.2k/s while reads drop from 4.5k/s to 1.8k/s.

| policy | hit ratio | vs LRU | misses |
|---|---|---|---|
| LRU (stock) | 13.60% | — | 640,804 |
| **WarmAll** | **20.50%** | **+6.90pp** | 403,055 |
| Leaper (prefetch only) | 15.86% | +2.26pp | 627,006 |
| Oracle | 16.45% | +2.85pp | 561,011 |

WarmAll's advantage *grows* to the largest value measured anywhere. It is also
worth recording why the experiment did not test what it was designed to test:
compaction output barely moved (1869 MB against 1833 MB at a third of the write
rate), because the updates rewrite the same narrow hot key range and LevelDB
trivially moves the append-only inserts without rewriting them. **On a
concentrated-hot-set workload, raising the write rate does not raise compaction
volume proportionally.** What did change is the read rate, which fell by 2.5x
and so lengthened the interval at which a hot block is re-read — and WarmAll
got better, not worse.

That is the cleanest confirmation of the mechanism: fewer reads means LRU has
less evidence about what is hot, so a prior based on *write* recency beats a
prior based on *read* recency by more.

## Summary of what was tested

| hypothesis | knob | result |
|---|---|---|
| The cache is too rich | 27% -> 10% -> 3% -> 1% of data | **refuted** — all prefetch gains shrink; ordering never changes |
| Misses are too cheap | NVMe -> emulated 200 us device | refuted as an explanation — warming matters more, ordering unchanged |
| The hot set is too dense | 100 -> 1000 key ranges (~3% hot) | **refuted** — WarmAll's lead over the oracle grows |
| Reads and writes share a hot set | write_corr 1.0 -> 0.5 -> 0.2 | **refuted** — flat |
| Prediction horizon is too short | T2 pinned, threshold lowered | partial — relaxing helps, but only toward WarmAll |
| Not enough compaction churn | write mix 25% -> 70% | not testable with this generator; WarmAll improved |

Six ways of trying to make selection pay, and none of them does. The mechanism
that survives every one of them: **WarmAll is a recency-of-write prior and LRU is
a recency-of-read prior, and on an LSM-tree the block a compaction just wrote is
the better bet.** Selection can only add value on top of that where warming
displaces something that would have been read, and in none of the regimes
measured here does it.
