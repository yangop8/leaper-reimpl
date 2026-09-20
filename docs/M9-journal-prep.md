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

ZIPPY_TABLE

## 3. Where this leaves the claims

TO_FILL
