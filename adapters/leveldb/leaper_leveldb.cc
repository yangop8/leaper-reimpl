// Copyright (c) 2026 The Leaper Authors. BSD-3-Clause (see LICENSE).

#include "leaper_leveldb.h"

#include <algorithm>
#include <chrono>

namespace leaper_leveldb {
namespace {
uint64_t MonotonicUs() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}
}  // namespace

// Translates leaper::CacheOps into the block-cache operations the patched
// LevelDB exposes. Warming is deferred: a block cannot be addressed in the
// cache until its file has been opened by the TableCache, which assigns the
// cache_id, so ShouldPrefetch only records the decision.
class Adapter::CacheBridge : public leaper::CacheOps {
 public:
  explicit CacheBridge(Adapter* a) : a_(a) {}

  void Evict(const leaper::BlockRef& b) override {
    if (a_->ops_ == nullptr) return;
    leaper::ScopedInternalCacheAccess guard;
    a_->ops_->EvictBlock(b.file_id, FileSize(b.file_id), b.offset);
  }
  void Prefetch(const leaper::BlockRef& b) override {
    if (a_->ops_ == nullptr) return;
    leaper::ScopedInternalCacheAccess guard;
    a_->ops_->WarmBlock(b.file_id, FileSize(b.file_id), b.offset, b.size);
  }
  bool IsCached(const leaper::BlockRef& b) override {
    if (a_->ops_ == nullptr) return false;
    leaper::ScopedInternalCacheAccess guard;
    return a_->ops_->IsBlockCached(b.file_id, FileSize(b.file_id), b.offset);
  }

 private:
  uint64_t FileSize(uint64_t file_number) const {
    auto it = a_->file_sizes_.find(file_number);
    return it == a_->file_sizes_.end() ? 0 : it->second;
  }
  Adapter* a_;
};

std::unique_ptr<Adapter> Adapter::Create(const AdapterOptions& opts,
                                         std::string* error) {
  std::unique_ptr<Adapter> a(new Adapter());
  a->start_us_ = MonotonicUs();
  a->mapper_ = (opts.key_format == "prefix")
                   ? leaper::NewPrefixRangeMapper(opts.core.range_size)
                   : leaper::NewDecimalRangeMapper(opts.core.range_size);
  a->max_range_id_ = opts.core.max_range_id;
  a->bridge_.reset(new CacheBridge(a.get()));
  a->core_ = leaper::Leaper::Open(opts.core, a->mapper_.get(), a->bridge_.get(),
                                  error);
  if (a->core_ == nullptr) return nullptr;
  return a;
}

Adapter::~Adapter() = default;

void Adapter::Bind(leveldb::LeaperEngineOps* ops) { ops_ = ops; }

void Adapter::ResetClock() { start_us_ = MonotonicUs(); }

uint64_t Adapter::NowUs() const { return MonotonicUs() - start_us_; }

leaper::RangeId Adapter::RangeOfInternalKey(const leveldb::Slice& k) const {
  return std::min(MapInternalKey(k), max_range_id_);
}

leaper::RangeId Adapter::MapInternalKey(const leveldb::Slice& k) const {
  // Index entries and TableBuilder bounds are internal keys: user key plus an
  // 8-byte (sequence, type) tag. FindShortestSeparator can shorten the user
  // key part but keeps a valid internal key, so stripping 8 bytes is correct.
  const size_t n = k.size() >= 8 ? k.size() - 8 : k.size();
  return mapper_->Map(k.data(), n);
}

void Adapter::OnGet(const leveldb::Slice& user_key) {
  core_->OnRead(user_key.data(), user_key.size(), NowUs());
}

void Adapter::OnPut(const leveldb::Slice& user_key) {
  core_->OnWrite(user_key.data(), user_key.size(), NowUs());
}

void Adapter::OnCompactionBegin(
    int level, bool is_flush,
    const std::vector<leveldb::LeaperBlockInfo>& inputs) {
  std::vector<leaper::BlockRef> blocks;
  blocks.reserve(inputs.size());
  {
    std::lock_guard<std::mutex> lock(mu_);
    for (const leveldb::LeaperBlockInfo& b : inputs) {
      leaper::BlockRef r;
      r.file_id = b.file_number;
      r.offset = b.offset;
      r.size = b.size;
      r.first_range = RangeOfInternalKey(b.smallest_key);
      r.last_range = RangeOfInternalKey(b.largest_key);
      if (r.last_range < r.first_range) std::swap(r.first_range, r.last_range);
      blocks.push_back(r);
    }
    pending_warm_.clear();
  }
  leaper::CompactionInfo info;
  info.level = level;
  info.is_flush = is_flush;
  info.est_blocks = blocks.size();
  core_->OnCompactionBegin(info, blocks, NowUs());
}

void Adapter::OnOutputFileStart(uint64_t file_number) {
  std::lock_guard<std::mutex> lock(mu_);
  current_output_ = file_number;
}

void Adapter::OnOutputBlock(const leveldb::LeaperBlockInfo& block) {
  leaper::BlockRef r;
  {
    std::lock_guard<std::mutex> lock(mu_);
    r.file_id = current_output_;
  }
  r.offset = block.offset;
  r.size = block.size;
  r.first_range = RangeOfInternalKey(block.smallest_key);
  r.last_range = RangeOfInternalKey(block.largest_key);
  if (r.last_range < r.first_range) std::swap(r.first_range, r.last_range);
  if (!core_->ShouldPrefetch(r, NowUs())) return;
  std::lock_guard<std::mutex> lock(mu_);
  pending_warm_.push_back(r);
}

void Adapter::OnOutputFileFinished(uint64_t file_number, uint64_t file_size) {
  std::vector<leaper::BlockRef> warm;
  {
    std::lock_guard<std::mutex> lock(mu_);
    file_sizes_[file_number] = file_size;
    for (leaper::BlockRef& r : pending_warm_) {
      if (r.file_id == file_number) warm.push_back(r);
    }
    pending_warm_.erase(
        std::remove_if(pending_warm_.begin(), pending_warm_.end(),
                       [&](const leaper::BlockRef& r) {
                         return r.file_id == file_number;
                       }),
        pending_warm_.end());
  }
  if (warm.empty()) return;
  const uint64_t t0 = MonotonicUs();
  for (const leaper::BlockRef& r : warm) bridge_->Prefetch(r);
  warm_us_ += MonotonicUs() - t0;
  warmed_ += warm.size();
}

void Adapter::OnCompactionEnd() {
  leaper::CompactionInfo info;
  core_->OnCompactionEnd(info, NowUs());
}

void Adapter::OnFileObsolete(uint64_t file_number, uint64_t file_size) {
  if (ops_ == nullptr) return;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (file_size > 0) file_sizes_[file_number] = file_size;
  }
  std::vector<leaper::BlockRef> blocks;
  ops_->ForEachDataBlock(file_number, file_size,
                         [&](const leveldb::LeaperBlockInfo& b) {
                           leaper::BlockRef r;
                           r.file_id = b.file_number;
                           r.offset = b.offset;
                           r.size = b.size;
                           blocks.push_back(r);
                         });
  core_->OnFileObsolete(file_number, blocks);
  std::lock_guard<std::mutex> lock(mu_);
  file_sizes_.erase(file_number);
}

}  // namespace leaper_leveldb
