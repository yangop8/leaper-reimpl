// Copyright (c) 2026 The Leaper Authors. BSD-3-Clause (see LICENSE).
//
// StatsCache: a decorator over leveldb::Cache that measures the block cache
// WITHOUT patching LevelDB. Options::block_cache is a Cache*, and in LevelDB
// only Table::BlockReader (table/table.cc) uses it, so everything this class
// sees is a data-block access.
//
// It also answers the question that motivates Leaper's phase-1 eviction on
// LevelDB specifically: LevelDB never erases block cache entries belonging to
// an SST that compaction deleted. DBImpl::RemoveObsoleteFiles only evicts the
// *table* cache entry (db/db_impl.cc:274); the data blocks keep sitting in the
// block cache under a cache_id that Table::Open (table/table.cc:72) will never
// hand out again, so they are unreachable garbage until LRU happens to evict
// them. We measure that directly by accounting live bytes per cache_id and
// tracking when each cache_id was last hit.
//
// Block cache key layout (table/table.cc:169-172), stable in LevelDB 1.23:
//   [0,8)  little-endian cache_id (Table::rep_->cache_id)
//   [8,16) little-endian block offset

#ifndef LEAPER_BENCH_STATS_CACHE_H_
#define LEAPER_BENCH_STATS_CACHE_H_

#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include "leaper/leaper.h"
#include "leveldb/cache.h"
#include "leveldb/slice.h"

namespace leaper_bench {

struct CacheCounters {
  uint64_t lookups = 0, hits = 0, misses = 0;
  uint64_t inserts = 0, insert_bytes = 0;
  uint64_t erases = 0, evictions = 0, evicted_bytes = 0;
  uint64_t live_bytes = 0;   // charge currently resident (tracking mode only)
  uint64_t stale_bytes = 0;  // resident bytes of cache_ids idle > threshold
  uint64_t stale_ids = 0;
  uint64_t internal_lookups = 0;   // Leaper's own probes, excluded above
  uint64_t prefetch_inserts = 0;   // cache inserts caused by prefetching
};

class StatsCache : public leveldb::Cache {
 public:
  // |track_cache_ids| enables per-table live-byte accounting. It costs one
  // heap allocation per cache insert and two atomics per access, so leave it
  // off for headline throughput numbers and on for cache-occupancy studies.
  StatsCache(leveldb::Cache* target, bool track_cache_ids)
      : target_(target), track_(track_cache_ids),
        live_(track_cache_ids ? kSlots : 0),
        last_hit_(track_cache_ids ? kSlots : 0) {
    for (size_t i = 0; i < live_.size(); ++i) {
      live_[i].store(0, std::memory_order_relaxed);
      last_hit_[i].store(0, std::memory_order_relaxed);
    }
  }

  ~StatsCache() override { delete target_; }

  Handle* Insert(const leveldb::Slice& key, void* value, size_t charge,
                 void (*deleter)(const leveldb::Slice&, void*)) override {
    if (leaper::InInternalCacheAccess()) {
      prefetch_inserts_.fetch_add(1, std::memory_order_relaxed);
    }
    inserts_.fetch_add(1, std::memory_order_relaxed);
    insert_bytes_.fetch_add(charge, std::memory_order_relaxed);
    if (!track_) return target_->Insert(key, value, charge, deleter);

    Entry* e = new Entry{value, deleter, this, CacheIdOf(key), charge};
    live_[Slot(e->cache_id)].fetch_add(charge, std::memory_order_relaxed);
    live_total_.fetch_add(charge, std::memory_order_relaxed);
    return target_->Insert(key, e, charge, &StatsCache::Dispatch);
  }

  Handle* Lookup(const leveldb::Slice& key) override {
    // Leaper's own probes must not be counted as workload accesses; see
    // leaper::InInternalCacheAccess.
    if (leaper::InInternalCacheAccess()) {
      internal_lookups_.fetch_add(1, std::memory_order_relaxed);
      return target_->Lookup(key);
    }
    lookups_.fetch_add(1, std::memory_order_relaxed);
    Handle* h = target_->Lookup(key);
    if (h != nullptr) {
      hits_.fetch_add(1, std::memory_order_relaxed);
      if (track_) {
        last_hit_[Slot(CacheIdOf(key))].store(now_secs_.load(std::memory_order_relaxed),
                                              std::memory_order_relaxed);
      }
    } else {
      misses_.fetch_add(1, std::memory_order_relaxed);
    }
    return h;
  }

  void Release(Handle* handle) override { target_->Release(handle); }

  void* Value(Handle* handle) override {
    void* v = target_->Value(handle);
    return track_ ? static_cast<Entry*>(v)->value : v;
  }

  void Erase(const leveldb::Slice& key) override {
    erases_.fetch_add(1, std::memory_order_relaxed);
    target_->Erase(key);
  }

  uint64_t NewId() override { return target_->NewId(); }
  void Prune() override { target_->Prune(); }
  size_t TotalCharge() const override { return target_->TotalCharge(); }

  // The reporter thread publishes wall-clock seconds here so the hot path can
  // stamp last-hit times with a single relaxed store.
  void SetClock(uint64_t secs) { now_secs_.store(secs, std::memory_order_relaxed); }

  // A cache_id idle for more than |stale_after_secs| is almost certainly a
  // compacted-away SST whose blocks can never be looked up again.
  CacheCounters Snapshot(uint64_t stale_after_secs) const {
    CacheCounters c;
    c.lookups = lookups_.load(std::memory_order_relaxed);
    c.hits = hits_.load(std::memory_order_relaxed);
    c.misses = misses_.load(std::memory_order_relaxed);
    c.inserts = inserts_.load(std::memory_order_relaxed);
    c.insert_bytes = insert_bytes_.load(std::memory_order_relaxed);
    c.erases = erases_.load(std::memory_order_relaxed);
    c.evictions = evictions_.load(std::memory_order_relaxed);
    c.evicted_bytes = evicted_bytes_.load(std::memory_order_relaxed);
    c.live_bytes = live_total_.load(std::memory_order_relaxed);
    c.internal_lookups = internal_lookups_.load(std::memory_order_relaxed);
    c.prefetch_inserts = prefetch_inserts_.load(std::memory_order_relaxed);
    if (track_) {
      const uint64_t now = now_secs_.load(std::memory_order_relaxed);
      for (size_t i = 0; i < live_.size(); ++i) {
        const uint64_t bytes = live_[i].load(std::memory_order_relaxed);
        if (bytes == 0) continue;
        const uint64_t seen = last_hit_[i].load(std::memory_order_relaxed);
        if (now > seen + stale_after_secs) {
          c.stale_bytes += bytes;
          c.stale_ids += 1;
        }
      }
    }
    return c;
  }

 private:
  struct Entry {
    void* value;
    void (*deleter)(const leveldb::Slice&, void*);
    StatsCache* owner;
    uint64_t cache_id;
    size_t charge;
  };

  // cache_ids come from Cache::NewId(), i.e. a dense increasing sequence, so
  // masking is collision-free for the first kSlots tables ever opened.
  static constexpr size_t kSlots = 1u << 20;
  static size_t Slot(uint64_t cache_id) { return cache_id & (kSlots - 1); }

  static uint64_t CacheIdOf(const leveldb::Slice& key) {
    uint64_t id = 0;
    if (key.size() >= 8) std::memcpy(&id, key.data(), 8);  // LE, per EncodeFixed64
    return id;
  }

  static void Dispatch(const leveldb::Slice& key, void* value) {
    Entry* e = static_cast<Entry*>(value);
    StatsCache* self = e->owner;
    self->evictions_.fetch_add(1, std::memory_order_relaxed);
    self->evicted_bytes_.fetch_add(e->charge, std::memory_order_relaxed);
    self->live_[Slot(e->cache_id)].fetch_sub(e->charge, std::memory_order_relaxed);
    self->live_total_.fetch_sub(e->charge, std::memory_order_relaxed);
    e->deleter(key, e->value);
    delete e;
  }

  leveldb::Cache* target_;
  const bool track_;

  std::atomic<uint64_t> lookups_{0}, hits_{0}, misses_{0};
  std::atomic<uint64_t> inserts_{0}, insert_bytes_{0}, erases_{0};
  std::atomic<uint64_t> evictions_{0}, evicted_bytes_{0}, live_total_{0};
  std::atomic<uint64_t> now_secs_{0};
  std::atomic<uint64_t> internal_lookups_{0}, prefetch_inserts_{0};

  std::vector<std::atomic<uint64_t>> live_;
  std::vector<std::atomic<uint64_t>> last_hit_;
};

}  // namespace leaper_bench

#endif  // LEAPER_BENCH_STATS_CACHE_H_
