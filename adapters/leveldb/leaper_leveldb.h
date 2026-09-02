// Copyright (c) 2026 The Leaper Authors. BSD-3-Clause (see LICENSE).
//
// LevelDB adapter. Implements leveldb::LeaperHooks on one side and
// leaper::CacheOps on the other; contains no policy of its own. Porting to
// another engine means writing this file again, not the core.

#ifndef LEAPER_ADAPTERS_LEVELDB_H_
#define LEAPER_ADAPTERS_LEVELDB_H_

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "leaper/leaper.h"
#include "leveldb/leaper_hooks.h"
#include "leveldb/options.h"

namespace leaper_leveldb {

struct AdapterOptions {
  leaper::Options core;
  // Key format: "decimal" for the benchmark's 16-byte zero-padded keys,
  // "prefix" for arbitrary byte keys (first 8 bytes, big-endian).
  std::string key_format = "decimal";
};

class Adapter : public leveldb::LeaperHooks {
 public:
  static std::unique_ptr<Adapter> Create(const AdapterOptions& opts,
                                         std::string* error);
  ~Adapter() override;

  void Bind(leveldb::LeaperEngineOps* ops) override;
  void OnGet(const leveldb::Slice& user_key) override;
  void OnSeek(const leveldb::Slice& user_key) override;
  void OnPut(const leveldb::Slice& user_key) override;
  void OnCompactionBegin(int level, bool is_flush,
                         const std::vector<leveldb::LeaperBlockInfo>& inputs) override;
  void OnOutputFileStart(uint64_t file_number) override;
  void OnOutputBlock(const leveldb::LeaperBlockInfo& block) override;
  void OnOutputFileFinished(uint64_t file_number, uint64_t file_size) override;
  void OnCompactionEnd() override;
  void OnFileObsolete(uint64_t file_number, uint64_t file_size) override;

  leaper::Stats stats() const { return core_->stats(); }
  void set_qps(double qps) { core_->set_qps(qps); }
  void set_health(double miss_ratio) { core_->set_health(miss_ratio); }
  // Aligns the plug-in's clock with the measurement window so that offline
  // artefacts indexed by trace time (the oracle's per-slot hot sets, and the
  // timestamp features) line up with what the plug-in sees online.
  void ResetClock();
  uint64_t warmed_blocks() const { return warmed_; }
  uint64_t warm_failed() const { return warm_failed_.load(); }
  uint64_t evict_failed() const { return evict_failed_.load(); }
  uint64_t warm_us() const { return warm_us_; }

 private:
  class CacheBridge;

  Adapter() = default;
  uint64_t NowUs() const;
  leaper::RangeId RangeOfInternalKey(const leveldb::Slice& internal_key) const;
  leaper::RangeId MapInternalKey(const leveldb::Slice& internal_key) const;

  std::unique_ptr<leaper::RangeMapper> mapper_;
  std::unique_ptr<CacheBridge> bridge_;
  std::unique_ptr<leaper::Leaper> core_;
  leveldb::LeaperEngineOps* ops_ = nullptr;
  uint64_t start_us_ = 0;
  leaper::RangeId max_range_id_ = 1u << 20;

  std::mutex mu_;
  uint64_t current_output_ = 0;
  std::vector<leaper::BlockRef> pending_warm_;
  // File sizes learned from compaction inputs, so an obsolete file can still
  // be opened to enumerate its blocks.
  std::unordered_map<uint64_t, uint64_t> file_sizes_;
  uint64_t warmed_ = 0, warm_us_ = 0;
  std::atomic<uint64_t> warm_failed_{0}, evict_failed_{0};
};

}  // namespace leaper_leveldb

#endif  // LEAPER_ADAPTERS_LEVELDB_H_
