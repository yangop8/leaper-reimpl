# M2-M3 — Online collector, inference, and the two-phase prefetcher on LevelDB

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

M0 established the phenomenon and M1 the offline model. This milestone puts the
model online: statistics collection on the client path, inference bound to
background operations, and the two-phase prefetcher of Section 6.

## What is engine-independent, and why that matters

Everything that is Leaper lives in `leaper/` and knows nothing about LevelDB:

| file | paper section |
|---|---|
| `src/collector.{h,cc}` | 5.1 statistics collection, locking, sampling |
| `src/gbdt.{h,cc}` | 4.3 model inference |
| `src/predictor.{h,cc}` | 4.2 features, 6.1 multi-step prediction |
| `src/overlap.{h,cc}` | Algorithm 3 overlap check |
| `src/leaper.cc` | 6.2 two-phase prefetcher, and the baseline policies |

`adapters/leveldb/` and `adapters/rocksdb/` translate. The RocksDB adapter
(M5-M7) links the same `leaper_core` without a line changed in it, which is the
only real test that the split is a split and not a decoration.

## Three implementation decisions worth defending

**No model runtime.** The paper compiles its model with Treelite for a 3-5x
inference speedup. We parse LightGBM's own text dump and walk the trees: ~200
lines, no dependency, and measured at **1.96-2.33 us per inference** on 18
features, which is not the bottleneck at a few hundred ranges per compaction.
Treelite stays available as an A/B for reproducing the speedup claim; it is not
needed for correctness.

**The scorer is verified against LightGBM, not trusted.** `leaper/src/gbdt_check.cc`
loads a model plus 2000 test rows with LightGBM's own predictions and compares.
A scorer that is subtly wrong would produce plausible-looking online numbers
that mean nothing. Measured agreement: **mean |diff| 3.7e-9, max 3.0e-8** over
2000 rows and 127 trees.

**Sampling always records the first access.** The paper's collector uses lazy
initialisation plus double-checked locking so that the *first* access to a key
range in an interval is always counted and only the ones after it are sampled;
the estimator is then `N = (S-1)/P + 1`. Uniform sampling would be much simpler
and quite wrong: a range accessed once per interval would vanish with
probability `1-P`, and its label — the thing the binary classifier predicts —
would flip. The counters pack `(slot_epoch << 32 | count)` into one atomic word
so a slot rolls over lazily on first touch, with no background sweep and a
single relaxed `fetch_add` on the common path.

## The LevelDB patch

LevelDB has no `EventListener` and no way to address a block cache entry from
outside (the key is `cache_id || offset`, and `cache_id` is assigned privately
in `Table::Open`). The patch adds the smallest surface that fixes that, with a
null default so a build without hooks is byte-for-byte stock:
`adapters/leveldb/leveldb-1.23-leaper-hooks.patch`, **277 lines across 8 files**
plus one new header.

| file | what it gains |
|---|---|
| `include/leveldb/leaper_hooks.h` | new: `LeaperHooks` + `LeaperEngineOps` |
| `include/leveldb/options.h` | `Options::leaper_hooks`, defaulting to null |
| `include/leveldb/table.h`, `table/table.cc` | `block_cache_id`, `ForEachDataBlock`, `WarmBlock`, `EvictBlock`, `IsBlockCached` |
| `db/table_cache.{h,cc}` | the same, keyed by file number |
| `db/db_impl.{h,cc}` | implements `LeaperEngineOps`; hooks on Get, Write, compaction begin/end, output files, obsolete files |
| `table/table_builder.cc` | tracks each block's first key and reports the block as it is written |

`TableBuilder::Flush` is the only point where a block's extent and both key
bounds are known together while its contents are still in memory, which is why
the hook is there rather than after the file is closed.

## Measuring the prefetcher without disturbing what it measures

`kIncrementalWarmup` calls `IsCached` once per input block, and those probes go
through the same block cache whose hit ratio is the headline metric. Counting
them as workload accesses moved the reported hit ratio from 0.60 to **0.468** —
a policy looked catastrophic because of the instrument, not the policy.
`leaper::ScopedInternalCacheAccess` marks the plug-in's own accesses so
`StatsCache` can exclude them; the adapter wraps every cache operation in it.

## T1 and T2 must be measured, not guessed

The two-phase split needs `T1 ~ alpha * N` (compaction duration) and
`T2 ~ beta * Q / S` (cache recovery time). The paper says both constants are
"computed by sampling from previous log data" and gives no values. A plausible
guess for beta was wrong by ten orders of magnitude, which made `T2` collapse to
a single interval; the second phase then read the weakest multi-step model and
the prefetcher predicted almost nothing hot (188 hot range-predictions across a
whole run, versus 726 after calibration).

`tools/calibrate_phases.py` fits both from a run's own compaction timeline and
hit-ratio recovery curve. On the M4 training workload:

```
[T1] 288 compactions, duration median=0.129s max=0.184s
[T1] alpha = 1.317e-05 seconds per block
[T2] 81 measured recoveries, median=1.726s p90=2.772s
[T2] beta = 5.767e+03
```

## Multi-step prediction degrades fast

One model per step, each predicting the k-th interval ahead. Measured on the M4
training workload:

| step | precision | recall |
|---|---|---|
| 1 | 0.986 | 0.807 |
| 2 | 0.908 | 0.616 |
| 3 | 0.784 | 0.466 |
| 4 | 0.650 | 0.293 |
| 5 | 0.612 | 0.258 |
| 6 | 0.597 | 0.251 |

Recall halves by step 3. This bounds how far ahead the two-phase prefetcher can
usefully look, and it is why getting `T1`/`T2` right matters so much: an
overestimated `T1` pushes phase 2 into models that barely beat chance.
