// Copyright (c) 2026 The Leaper Authors. BSD-3-Clause (see LICENSE).
//
// Leaper: a learned prefetcher for cache invalidation in LSM-tree storage
// engines. Clean-room reimplementation of PVLDB 13(11):1976-1989.
//
// This header is the whole engine-facing contract. Nothing below knows about
// LevelDB or RocksDB; the adapters in adapters/<engine>/ translate. Porting to
// a new engine means implementing CacheOps and calling the hooks -- not
// reimplementing the collector, the model, or the two-phase policy.

#ifndef LEAPER_LEAPER_H_
#define LEAPER_LEAPER_H_

#include <cstdint>
#include <memory>
#include <string>
#include <string>
#include <vector>

namespace leaper {

// ---------------------------------------------------------------------------
// Policies. Leaper is one of several; the others exist so that its measured
// gain can be attributed. See docs/M4-baselines.md.
enum class Policy {
  kOff,               // stock engine, LRU only
  kEagerEvict,        // drop cache entries of SSTs compaction deleted
  kIncrementalWarmup, // paper's baseline: warm new blocks overlapping cached ones
  kWarmAll,           // warm every newly written block (= RocksDB kFlushAndCompaction)
  kWarmFlush,         // warm flush outputs only (= RocksDB kFlushOnly)
  kLeaper,            // learned two-phase prefetcher
  kOracle,            // replay of future accesses; upper bound, offline only
};

const char* PolicyName(Policy p);
bool ParsePolicy(const std::string& name, Policy* out);

// ---------------------------------------------------------------------------
// A half-open key range [begin, end) in *range id* space, and the block
// metadata the overlap check works on. Keys reach Leaper already mapped to
// range ids, so nothing here depends on the engine's key encoding.
using RangeId = uint64_t;

struct BlockRef {
  uint64_t file_id = 0;   // engine's file identity (LevelDB file number)
  uint64_t offset = 0;    // block offset within the file
  uint64_t size = 0;
  RangeId first_range = 0;
  RangeId last_range = 0; // inclusive
};

// Leaper probes and mutates the very cache whose hit ratio is being measured
// (kIncrementalWarmup calls IsCached once per input block). Instrumentation
// that counts those probes as workload accesses reports a hit ratio that is
// wrong by however much the policy pokes at the cache -- measured at 0.468 vs
// a true 0.60 on a smoke run. The adapter marks its own accesses so the
// instrumentation can exclude them.
bool InInternalCacheAccess();

class ScopedInternalCacheAccess {
 public:
  ScopedInternalCacheAccess();
  ~ScopedInternalCacheAccess();
  ScopedInternalCacheAccess(const ScopedInternalCacheAccess&) = delete;
  ScopedInternalCacheAccess& operator=(const ScopedInternalCacheAccess&) = delete;
};

// What the adapter must provide so Leaper can act on the engine's block cache.
class CacheOps {
 public:
  virtual ~CacheOps() = default;
  // Drop a block from the cache if present. Must be safe on a live entry.
  virtual void Evict(const BlockRef& block) = 0;
  // Bring a block into the cache. The adapter decides whether that means an
  // insert of already-in-memory contents or a read through the normal path.
  virtual void Prefetch(const BlockRef& block) = 0;
  // Is this block currently cached? Used by kIncrementalWarmup.
  virtual bool IsCached(const BlockRef& block) = 0;
};

// ---------------------------------------------------------------------------
// Order-preserving key -> range id. Order preservation is not optional: the
// overlap check (Algorithm 3) binary-searches block boundaries against range
// boundaries, which is only valid if the mapping is monotone.
class RangeMapper {
 public:
  virtual ~RangeMapper() = default;
  virtual RangeId Map(const char* key, size_t len) const = 0;
  virtual uint64_t range_size() const = 0;
  // First key of a range. Engines that cannot address blocks directly warm
  // the cache by seeking here and scanning forward, so the inverse has to
  // exist -- another reason the mapping must be monotone.
  virtual std::string RangeStartKey(RangeId range) const = 0;
};

// Interprets the first 8 bytes of the key as a big-endian integer and divides.
// Monotone for fixed-width keys and for any prefix-ordered encoding.
std::unique_ptr<RangeMapper> NewPrefixRangeMapper(uint64_t range_size);
// Fixed-width zero-padded decimal keys (the benchmark's 16-byte format).
//
// |key_width| is not cosmetic. LevelDB stores *physically shortened* keys in
// the index block: InternalKeyComparator::FindShortestSeparator truncates the
// user key and increments its last byte (db/dbformat.cc:72-77). A 16-digit key
// 0000000000123456 can appear in the index as the 14-byte string
// 00000000001235. Parsing that as an integer yields 1235 instead of 123500 --
// wrong by a factor of 100, and systematically biased towards range 0. Every
// block bound taken from an index is affected, which corrupts the candidate
// set and makes phase-1 eviction throw away the wrong blocks.
//
// Zero-padding a truncated separator back to |key_width| is exact, not an
// approximation: the separator S satisfies last_key <= S < next_first_key as
// byte strings, and padding with '0' (the smallest digit) preserves both
// inequalities against fixed-width keys.
std::unique_ptr<RangeMapper> NewDecimalRangeMapper(uint64_t range_size,
                                                   int key_width = 16);

// ---------------------------------------------------------------------------
struct Options {
  Policy policy = Policy::kOff;

  // Statistics collection (Section 5.1).
  double slot_seconds = 1.0;      // statistical time interval t
  int history_slots = 6;          // arrival-rate feature length
  double sample_rate = 1.0;       // Section 5.1 sampling; 1.0 disables it
  uint64_t range_size = 40000;    // keys per range; from Algorithm 1 offline
  size_t max_ranges = 1u << 22;   // counter table capacity
  // Highest valid range id. Block key bounds come from index entries, and
  // LevelDB's last index entry per SST is produced by FindShortSuccessor
  // (util/comparator.cc:54-64), which increments the first byte and truncates:
  // the 16-digit key 0000000000123456 becomes the single character "1".
  // Restored to full width that is 10^15, i.e. range id 25 billion. Expanding
  // such a span into individual ids allocates until the process is killed --
  // which is exactly what happened before this clamp existed.
  RangeId max_range_id = 1u << 20;

  // Model (Section 4.3). Multi-step prediction uses one model per step; a
  // single model is reused for every step when only one path is given.
  std::vector<std::string> model_paths;  // LightGBM text models, step 1..k
  std::string precursor_path;            // range -> precursors, from Algorithm 2
  std::string oracle_path;               // kOracle only: hot ranges per slot
  double hot_threshold = 0.5;

  // Two-phase prefetcher (Section 6.2). T1 ~ alpha * blocks_to_merge,
  // T2 ~ beta * QPS / cache_size; both constants are calibrated offline.
  // Calibrate these from a measured run with tools/calibrate_phases.py; the
  // paper computes them "by sampling from previous log data" and a guess is
  // not survivable. A beta that is wrong by orders of magnitude collapses T2
  // to one interval, which sends the second phase to the weakest multi-step
  // model and makes the prefetcher predict almost nothing hot.
  double t1_alpha = 2.77e-5;      // seconds per block merged
  double t2_beta = 3709.0;        // seconds per (op/s per cache byte)
  // Non-zero pins the phase length directly, bypassing the formula above.
  double t1_seconds = 0.0;
  double t2_seconds = 0.0;
  double cache_bytes = 512.0 * 1024 * 1024;

  // Budget guards. Prefetching more than this share of the cache in one
  // compaction is refused; without it a mispredicting model can evict the
  // working set it is supposed to protect.
  double max_prefetch_frac = 1.0;

  // Phase 1 (eviction) and phase 2 (prefetch) can be enabled separately. The
  // two do very different things -- one gives cache back, the other spends it
  // -- and attributing a combined result to "Leaper" without knowing which
  // half moved the number is not a measurement.
  bool enable_phase1 = true;
  bool enable_phase2 = true;

  // SSAD (paper Section 7.4): a rollback that switches the prefetcher off when
  // the system looks unhealthy, until the model is refreshed. The paper keys it
  // on slow-SQL counts; here the health signal is the block cache miss ratio
  // the harness reports each interval. 0 disables it.
  double ssad_miss_threshold = 0.0;   // absolute miss ratio; 0 disables
  // Relative mode: suspend when the miss ratio exceeds its own exponential
  // moving average by this fraction. An absolute threshold set below a
  // workload's steady-state miss ratio (0.7 on a workload that sits at 0.75)
  // switches the prefetcher off permanently, which tests nothing.
  double ssad_relative = 0.0;         // e.g. 0.3 = 30% above the EWMA; 0 disables
  double ssad_ewma_alpha = 0.05;
  int ssad_window = 5;             // consecutive intervals above/below to flip

  bool verbose = false;
};

// ---------------------------------------------------------------------------
struct CompactionInfo {
  std::vector<uint64_t> input_files;
  uint64_t est_blocks = 0;       // blocks to be merged, drives the T1 estimate
  int level = 0;
  bool is_flush = false;
};

struct Stats {
  uint64_t reads_seen = 0, writes_seen = 0, sampled = 0;
  uint64_t inferences = 0, inference_us = 0;
  uint64_t ranges_predicted_hot = 0, ranges_predicted_cold = 0;
  uint64_t blocks_prefetched = 0, blocks_evicted = 0;
  uint64_t overlap_checks = 0, overlap_us = 0;
  uint64_t prefetch_refused_budget = 0;
  // The paper's own quantity (Formulation 2): blocks of a compaction's inputs
  // that were resident in the cache when it started, summed over compactions.
  uint64_t invalidated_blocks = 0;
  uint64_t compactions_seen = 0;
  uint64_t ssad_suspensions = 0;   // times the prefetcher was switched off
  bool ssad_suspended = false;
};

// The plug-in. Thread-safe: OnRead/OnWrite are called from client threads,
// the compaction hooks from background threads.
class Leaper {
 public:
  static std::unique_ptr<Leaper> Open(const Options& options,
                                      RangeMapper* mapper,
                                      CacheOps* cache,
                                      std::string* error);
  virtual ~Leaper() = default;

  // Statistics collection, on the client path. Must be cheap.
  virtual void OnRead(const char* key, size_t len, uint64_t now_us) = 0;
  virtual void OnWrite(const char* key, size_t len, uint64_t now_us) = 0;

  // Phase 1 (eviction phase). Called when a compaction starts, with the blocks
  // of its input files. Blocks predicted cold for the duration T1 of the
  // compaction are evicted now; predicted-hot ones stay, because the inputs
  // remain readable until the new version is installed.
  virtual void OnCompactionBegin(const CompactionInfo& info,
                                 const std::vector<BlockRef>& input_blocks,
                                 uint64_t now_us) = 0;

  // Phase 2 (prefetch phase). Called for each block as compaction writes it.
  // Returns true if the block should be warmed into the cache.
  virtual bool ShouldPrefetch(const BlockRef& block, uint64_t now_us) = 0;

  virtual void OnCompactionEnd(const CompactionInfo& info, uint64_t now_us) = 0;

  // Called when an SST is deleted, for kEagerEvict.
  virtual void OnFileObsolete(uint64_t file_id,
                              const std::vector<BlockRef>& blocks) = 0;

  virtual Stats stats() const = 0;
  virtual void set_qps(double qps) = 0;
  // Health signal for SSAD, e.g. the last interval's block cache miss ratio.
  virtual void set_health(double miss_ratio) = 0;
  // Called with the number of input blocks that were cache-resident when a
  // compaction began; the adapter measures it, the core just records it.
  virtual void RecordInvalidation(uint64_t cached_input_blocks) = 0;
};

}  // namespace leaper

#endif  // LEAPER_LEAPER_H_
