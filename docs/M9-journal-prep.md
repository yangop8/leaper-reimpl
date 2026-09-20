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
to an offset, and the query rate follows a sine wave of period 86 s
(`--sine_a/b/d`). The port follows `db_bench`'s `GenerateTwoTermExpKeys` step
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

**The +11pp is not prefetching, and it took two more runs to see why.**
The Leaper row is the only one whose QPS exceeds the harness's rate budget
and whose compaction count is a third of everyone else's. Moving the warm
reads off the compaction thread (H11's `--warm_async`) changed nothing, so
the reads were not what was slowing compaction. The run's own statistics
were: 44.0M inferences at 4.95 us each — **217.6 s of the 300 s run**, on
LevelDB's one background thread. The model was predicting over 25,000
ranges, and every flush and every L0 compaction has inputs spanning the
whole key space, so each of them asked for 25,000 ranges times k1 + k2
steps. With 72% of the compaction thread's time spent in inference, the
engine completed 671 compactions instead of 1,783, invalidated a third as
much cache, and the hit ratio rose for a reason that has nothing to do with
what was warmed; meanwhile writes stalled behind the compaction backlog and
were repaid in bursts (78,936 ops in one second against a 60,000 budget),
which is the QPS excess. RocksDB paid the same inference (51M at 3.5 us,
180 s) but has two background threads and a hundredth of the compaction
work, so there it did not bind.

Two lessons for the write-up and one for the design. Any policy's cost on
the background thread is a confound on a compaction-bound engine, because
slowing compaction reduces invalidation — a hit-ratio gain that is really a
write-stall loss, and the sort of thing that only the compaction count and
the QPS column expose. The paper's Table 5 inference figure (1-5 ms per
compaction) assumes a few hundred candidate ranges; at 25,000 it is 300 ms
per job, and on an engine whose flushes span the whole key space that is
the wrong place to run it. The design consequence is that at fine
granularity inference must come off the compaction thread — predict
asynchronously at Begin and let the builder consult a ready answer — or
the candidate set must be bounded to what the job's output can actually
contain.

CTL_TABLE

## 3. Where this leaves the claims

TO_FILL
