# M0 — Reproducing the cache invalidation problem on stock LevelDB

> **Hit-ratio caveat (M8, 2026-09-02).** Every block cache hit ratio in this
> file was measured while the harness counted LevelDB's own compaction input
> reads as workload lookups (about a third of all lookups, nearly all misses).
> Hit-ratio *dips around compactions* are therefore partly the compaction
> thread's own misses, not only the workload's. The counting is fixed and the
> corrected measurements are in `docs/M8-review-followup.md`, section H.
> Latencies, QPS and compaction timelines here are unaffected.

> **Scope note.** These measurements come from a clean-room reimplementation on
> LevelDB/RocksDB with synthetic workloads and NVMe (slow storage is emulated).
> The paper's results come from X-Engine, real Tmall and DingTalk traffic, and
> spinning disks. The numbers below describe *this* setup and are not a
> replication of the paper's. See the top of the repository README.

Leaper (VLDB'20, PVLDB 13(11):1976-1989) was implemented and evaluated inside
X-Engine. This milestone establishes the *baseline* the open-source
reimplementation will be measured against: does the cache invalidation problem
the paper describes actually occur in LevelDB, and is it observable?

M0 changes **no LevelDB source file**. Everything is an observer attached
through LevelDB's public extension points, so the numbers are a valid
description of stock behaviour.

## Instrumentation

| Component | Extension point | What it measures |
|---|---|---|
| `StatsCache` | `Options::block_cache` (a `Cache*`) | block cache lookups/hits/inserts/evictions; live bytes per `cache_id` |
| `EventLogger` | `Options::info_log` (a `Logger*`) | compaction/flush/trivial-move timeline, parsed from the messages `DBImpl` already emits |
| `PreadEnv` | `Options::env` (an `Env*`) | forces the pread path; optionally bypasses the OS page cache |
| `leaper_bench` | — | zipfian workload driver, per-second QPS / hit ratio / latency percentiles |

## Finding 1 — on 64-bit POSIX, stock LevelDB's block cache is dead code

`util/env_posix.cc:46` mmaps the first `g_mmap_limit = 1000` SST files. When a
block is read from an mmap'd file, `ReadBlock` sees that the file returned a
pointer into memory it owns and marks the block non-cachable to avoid
double-caching:

```c++
// table/format.cc:99-106
if (data != buf) {
  // File implementation gave us pointer to some other data.
  delete[] buf;
  result->data = Slice(data, n);
  result->heap_allocated = false;
  result->cachable = false;  // Do not double-cache
}
```

`Table::BlockReader` (`table/table.cc:180`) only inserts when
`contents.cachable && options.fill_cache`, so it never inserts. Measured on
LevelDB 1.23 with a 50 MB database and an 8 MB block cache:

```
[summary] block cache: 2072088 lookups, 0 hits (0.0000), 0 inserts, 0 evictions
```

Two million lookups, zero hits, zero inserts — caching was being done entirely
by the OS page cache. **Any block-cache study on LevelDB must take it off the
mmap path first.** `PreadEnv` does that with an `EnvWrapper`, which also makes
LevelDB comparable to RocksDB (`allow_mmap_reads` defaults to false there), and
matters for the LevelDB → RocksDB port in M5-M7. With `PreadEnv` the same
configuration yields 43.5% hit ratio, 750k inserts, 747k evictions.

## Finding 2 — LevelDB never reclaims blocks of compacted-away SSTs

The block cache key is `cache_id (8B) || offset (8B)` (`table/table.cc:169-172`),
where `cache_id` comes from `block_cache->NewId()` at `Table::Open`
(`table/table.cc:72`). When compaction deletes an input SST,
`DBImpl::RemoveObsoleteFiles` calls `table_cache_->Evict(number)`
(`db/db_impl.cc:274`), which destroys the `Table` — but the data blocks stay in
the block cache under a `cache_id` that will never be handed out again. They
are unreachable garbage occupying cache until LRU happens to evict them.

Consequences for the port:

1. It is a real, non-learned win to evict them eagerly. That must be
   implemented as a separate baseline (**EagerEvict**), or Leaper's measured
   gain on LevelDB will be contaminated by a benefit that has nothing to do
   with the learned prefetcher.
2. It gives phase 1 of the two-phase prefetcher a second job on LevelDB that it
   did not have in X-Engine: during the compaction the input SSTs are still
   live and readable, so predicted-hot blocks must be *kept*; only once the new
   version is installed do the leftovers become garbage.

`StatsCache` measures this directly by accounting live bytes per `cache_id` and
recording when each `cache_id` was last hit. A `cache_id` idle for longer than
`--stale_after` seconds is reported as stale.

## Finding 3 — a closed-loop driver cannot show the phenomenon

With no rate limiting, write pressure tracks read throughput: the faster reads
go, the faster the memtable fills, and the database sits in permanent
compaction. Measured at 8 threads with a 1 GB database: 375k QPS, 3-4
compactions *per second*, i.e. no quiet period to contrast a compaction period
against. The paper's setting is the opposite — roughly 10 background operations
in a 200 s run.

`--write_rate` paces writes against the wall clock and spends the surplus
budget on reads, so the compaction cadence is set directly and the timeline has
visible quiet periods.

## Finding 4 — seek-triggered compaction exists but did not fire here

LevelDB gives every SST a seek budget of `file_size / 16384`
(`db/version_set.cc:663`) and schedules a compaction once a file exhausts it
(`Version::UpdateStats`, `db/version_set.cc:402-413`), which suggests that a
read-heavy workload could drive compaction on its own. **It did not.** Run C —
300 s of pure reads at 333k QPS — completed **0 compactions and 0 flushes**.

The reason is that a seek is charged only when a `Get` probes a *second* file:

```c++
// db/version_set.cc:344-349
if (state->stats->seek_file == nullptr && state->last_file_read != nullptr) {
  // We have had more than one seek for this read.  Charge the 1st file.
  state->stats->seek_file = state->last_file_read;
  ...
}
```

On a fully compacted database with non-overlapping levels and bloom filters,
almost every `Get` finds its key in the first file it opens, so nothing is ever
charged. All compaction observed in runs A and B was write-driven.

This matters for the design: **the compaction cadence is controlled by
`--write_rate` alone**, which makes the workload reproducible, and the LevelDB
port does not have to model a read-driven feedback loop. It also means run C is
a valid *control*: the same reads, the same cache, no background operations.

## Finding 5 — the storage medium is the latency amplifier, and it is gone

The paper's headline result includes eliminating 99% of latency spikes, with
10x p95 excursions in Figure 1. That amplification comes from the storage the
original system ran on: spinning disks, where a block cache miss costs
milliseconds and the hit ratio takes a long time to recover afterwards. On the
NVMe used here a miss costs tens of microseconds, so the same hit-ratio drop
produces almost no tail movement — measured `corr(compactions_done,
read_p99_us) = -0.218`, i.e. no spike at all.

This does not weaken the reproduction, but it does mean **the latency result
cannot be reproduced on modern local NVMe at all**, and any claim about latency
has to be made in a regime where a miss is expensive. `--read_delay_us` adds a
fixed delay to every `RandomAccessFile::Read`, including compaction reads,
which recreates that regime deterministically and is far more reproducible than
sourcing a spinning disk. Plan for M4:

* `--read_delay_us=0`      NVMe, as measured here — report hit ratio and I/O
* `--read_delay_us=200`    SATA SSD / network block store
* `--read_delay_us=5000`   7.2k RPM disk, the paper's regime

## Workload dynamics

A stationary zipfian stream cannot evaluate the model. Every key range's
arrival rate is constant, so the paper's own baseline — "read in the last
interval implies read in the next" — is already optimal, and any learned model
can at best tie it. The paper's workloads are not stationary: an e-commerce hot
set slides forward under an auto-increment primary key, and volume is periodic
over a day. Both are reproduced by the driver:

| Flag | Effect | Which feature it exercises |
|---|---|---|
| `--hotspot_shift=<keys/s>` | hot region advances linearly | read/write arrival rates — the naive rule is systematically late at the moving frontier |
| `--phases=K --phase_period_s=P` | hot region rotates between K regions | the timestamp features, which otherwise carry no signal |
| `--key_dist=scrambled` | destroys range contiguity | ablation: shows range-level prediction depends on it |

Precursor structure (Algorithm 2) has no synthetic generator yet; on these
workloads the precursor features are expected to contribute nothing, and that
should be reported rather than hidden. Validating them needs either a
correlated-range generator or a real trace.

## Configuration notes

* Cache/data ratio matters. The paper uses 3 GB of block cache over 10 GB of
  data (~30%). At 6% the hot set does not fit and the hit ratio collapses to
  ~37%, which is a different regime from the one the paper studies.
* Updates hide the hot set from the block cache. With a skewed update stream,
  hot records are served out of the memtable and never reach the block cache,
  so the cache only sees the tail of the distribution. Keep the update rate in
  the range the paper uses (20%) and pace it.
* `--key_dist=zipf` keeps the hot set **contiguous** in key space, unlike
  YCSB's ScrambledZipfian. Leaper predicts at key-range granularity, so
  scrambling would destroy exactly the structure under study. `scrambled` is
  kept as an ablation.

## Results

16M records (~1.9 GB), 512 MB block cache (~27%), zipf 0.99 over a contiguous
hot region, 300 s measured after 30 s of warmup, NVMe, page cache bypassed.

| | threads | writes/s | compactions | flushes | mean hit ratio | mean QPS |
|---|---|---|---|---|---|---|
| A | 8 | 12k | 313 | 32 | 75.0% | 776k |
| B | 2 | 12k | 530 | 32 | 68.6% | 422k |
| C | 2 | 0 | **0** | **0** | **89.2%** | 333k |

C is the control: identical reads and cache, no background operations.

**The cache invalidation problem, quantified.** Going from C to B — same
database, same cache, same read distribution, only background operations added
— the block cache hit ratio falls from **89.2% to 68.6%**, i.e. the miss rate
roughly triples (10.8% → 31.4%).

Within run B the effect tracks compaction intensity second by second:

| | hit ratio | QPS | read p99 |
|---|---|---|---|
| heaviest 20% of compaction seconds | 62.68% | 343k | 97 µs |
| lightest 20% of compaction seconds | 72.01% | 473k | 89 µs |
| correlation with compactions/s | **−0.685** | **−0.799** | **+0.801** |

Run A, at 8 threads, shows the same effect more weakly (hit ratio correlation
−0.411, QPS −0.348, p99 −0.109): the foreground saturates the CPU and masks it.
**The phenomenon is easier to see when the client is not the bottleneck**, which
is worth remembering when configuring the M2-M4 comparisons — an over-driven
client will understate whatever Leaper recovers.

B also completed *more* compaction work than A (41.6 GB vs 24.4 GB in the same
300 s at the same write rate), because LevelDB's single background thread gets
more of the machine when the foreground asks for less.

## Running it

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

./build/leaper_bench \
  --db=/tmp/m0_db --num=16000000 --value_size=100 \
  --cache_mb=512 --write_buffer_mb=16 --max_file_mb=8 \
  --threads=8 --read_ratio=0.75 --update_ratio=0.20 --zipf=0.99 \
  --write_rate=12000 --duration=300 --warmup=30 \
  --out_prefix=experiments/results/m0_baseline

.venv/bin/python tools/plot_timeseries.py experiments/results/m0_baseline
```

Outputs `<prefix>.timeseries.csv` (per second), `<prefix>.events.csv`
(background operation timeline), `<prefix>.leveldb.log` (raw info log) and
`<prefix>.png`.
