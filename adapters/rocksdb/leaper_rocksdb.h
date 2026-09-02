// Copyright (c) 2026 The Leaper Authors. BSD-3-Clause (see LICENSE).
//
// RocksDB adapter. Same leaper_core as the LevelDB adapter -- same collector,
// same LightGBM scorer, same multi-step prediction, same two-phase policy --
// with a different translation layer. That the core is untouched is the point
// of the split.
//
// Two differences from the LevelDB adapter, both forced by the engine and both
// worth stating rather than hiding:
//
//  1. NO CORE PATCH IS NEEDED. RocksDB ships EventListener, which is exactly
//     the compaction visibility LevelDB had to be patched for.
//
//  2. NO PHASE 1. Leaper's eviction phase needs to address individual entries
//     in the block cache. RocksDB derives those keys from a per-file
//     OffsetableCacheKey that is not reachable from outside the table reader,
//     so eviction is not implementable as a plug-in. Prefetching is done at
//     key-range granularity by seeking with fill_cache on, which warms exactly
//     the blocks covering a predicted-hot range.
//
// RocksDB mainline already ships the unconditional form of the prefetch side
// (prepopulate_block_cache = kFlushAndCompaction), so the question here is not
// "can the cache be warmed" but "does choosing what to warm beat warming
// everything" -- which is why the evaluation compares against both.

#ifndef LEAPER_ADAPTERS_ROCKSDB_H_
#define LEAPER_ADAPTERS_ROCKSDB_H_

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "leaper/leaper.h"
#include "rocksdb/db.h"
#include "rocksdb/listener.h"

namespace leaper_rocksdb {

struct AdapterOptions {
  leaper::Options core;
  std::string key_format = "decimal";
  // Keys scanned forward from a hot range's first key when warming it. One
  // 4 KiB block holds roughly 35 records at the benchmark's value size, so the
  // default covers a whole range of 40k keys only partially on purpose: the
  // cost of warming has to stay bounded or the "prefetch" becomes a full scan.
  int warm_scan_keys = 4096;
  // Number of key ranges the database spans; the plug-in predicts over these.
  uint64_t num_ranges = 1024;
};

class Adapter {
 public:
  static std::unique_ptr<Adapter> Create(const AdapterOptions& opts,
                                         std::string* error);
  ~Adapter();

  // Install before opening the DB, then hand the DB back.
  std::shared_ptr<rocksdb::EventListener> listener();
  void SetDB(rocksdb::DB* db);
  void ResetClock();

  // RocksDB has no read hook, so the collector is driven from the client. In a
  // real deployment this would sit in DBImpl::GetImpl; here it stays out of
  // tree so the engine is stock.
  void OnRead(const rocksdb::Slice& key);
  void OnWrite(const rocksdb::Slice& key);

  void set_qps(double qps);
  void set_health(double miss_ratio);
  leaper::Stats stats() const;
  uint64_t warmed_ranges() const;
  uint64_t warm_us() const;

 private:
  class CacheBridge;
  class Listener;

  Adapter() = default;
  uint64_t NowUs() const;
  uint64_t NumRanges() const;

  std::unique_ptr<leaper::RangeMapper> mapper_;
  std::unique_ptr<CacheBridge> bridge_;
  std::unique_ptr<leaper::Leaper> core_;
  std::shared_ptr<Listener> listener_;
  rocksdb::DB* db_ = nullptr;
  uint64_t start_us_ = 0;
  int warm_scan_keys_ = 4096;
  uint64_t range_size_ = 1;
  uint64_t num_ranges_ = 1024;

  mutable std::mutex mu_;
  uint64_t warmed_ = 0, warm_us_ = 0;
  std::vector<leaper::BlockRef> pending_;  // decided at Begin, warmed at End
};

}  // namespace leaper_rocksdb

#endif  // LEAPER_ADAPTERS_ROCKSDB_H_
