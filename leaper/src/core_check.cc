// Copyright (c) 2026 The Leaper Authors. BSD-3-Clause (see LICENSE).
//
// core_check: regression tests for the core's job handling, each pinned to a
// defect from the 2026-09-19 review (docs/code-review-2026-09-19.md).
//
//   1. Nested background jobs. A flush that begins inside a compaction must
//      not replace the compaction's prediction or leave its flush flag set.
//   2. Flush candidates. A flush reported with the memtable's key span must
//      produce a prediction, so its output blocks can be prefetched.
//   3. Overlapping input blocks. SelectOverlapping must not skip a block that
//      is nested inside an earlier one.
//   7. Single model. One model stands in for every step, including the
//      compaction prefetch phase whose steps start at 2.
//
// The model used is a hand-written LightGBM text file with one single-leaf
// tree, so it predicts "hot" for every range; what is under test is the
// plumbing around it, not the prediction.
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include "leaper/leaper.h"
#include "overlap.h"

namespace {

int g_failed = 0;
#define CHECK(cond, msg)                                              \
  do {                                                                \
    if (!(cond)) {                                                    \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg); \
      ++g_failed;                                                     \
    }                                                                 \
  } while (0)

class RecordingCache : public leaper::CacheOps {
 public:
  void Evict(const leaper::BlockRef&) override {}
  void Prefetch(const leaper::BlockRef&) override { ++prefetched; }
  bool IsCached(const leaper::BlockRef&) override { return false; }
  int prefetched = 0;
};

leaper::BlockRef Block(leaper::RangeId first, leaper::RangeId last, uint64_t size = 4096) {
  leaper::BlockRef b;
  b.file_id = 1;
  b.offset = first * 4096;
  b.size = size;
  b.first_range = first;
  b.last_range = last;
  return b;
}

std::string WriteAlwaysHotModel(const std::string& dir, int n_features) {
  const std::string path = dir + "/always_hot.txt";
  std::ofstream out(path);
  out << "tree\nversion=v4\nnum_class=1\nnum_tree_per_iteration=1\nlabel_index=0\n"
      << "max_feature_idx=" << (n_features - 1) << "\nobjective=binary sigmoid:1\n"
      << "feature_names=";
  for (int i = 0; i < n_features; ++i) out << (i ? " f" : "f") << i;
  out << "\ntree_sizes=1\n\nTree=0\nnum_leaves=1\nnum_cat=0\nshrinkage=1\n"
      << "leaf_value=6.0\n\nend of trees\n";
  return path;
}

std::unique_ptr<leaper::Leaper> OpenCore(leaper::Policy policy, const std::string& model,
                                         leaper::RangeMapper* mapper, RecordingCache* cache) {
  leaper::Options o;
  o.policy = policy;
  o.range_size = 1000;
  o.max_range_id = 1000;
  o.slot_seconds = 1.0;
  o.t1_seconds = 2.0;  // k1 = 2, so the compaction prefetch phase is steps 3..
  o.t2_seconds = 3.0;  // k2 = 3
  o.cache_bytes = 64.0 * 1024 * 1024;
  if (!model.empty()) o.model_paths.push_back(model);
  std::string err;
  std::unique_ptr<leaper::Leaper> core = leaper::Leaper::Open(o, mapper, cache, &err);
  if (core == nullptr) {
    std::fprintf(stderr, "Leaper::Open failed: %s\n", err.c_str());
    std::exit(2);
  }
  return core;
}

// ---------------------------------------------------------------------------
void TestOverlapNested() {
  // Review's minimal case: blocks [0,10] and [1,2], hot ranges {0,2,4}.
  std::vector<leaper::BlockRef> blocks = {Block(0, 10), Block(1, 2)};
  std::vector<leaper::RangeSpan> spans = leaper::ToSpans({0, 2, 4});
  std::vector<size_t> sel = leaper::SelectOverlapping(blocks, spans);
  bool has1 = false;
  for (size_t i : sel) has1 |= (i == 1);
  CHECK(has1, "nested block [1,2] overlapping hot range 2 was not selected");

  // Randomised cross-check of the merge path against the per-block binary
  // search, with overlapping and nested blocks, sorted by first_range.
  std::mt19937 rng(7);
  for (int trial = 0; trial < 500; ++trial) {
    const int m = 1 + rng() % 60, n_hot = 1 + rng() % 40;
    std::vector<leaper::BlockRef> bs;
    for (int i = 0; i < m; ++i) {
      const leaper::RangeId a = rng() % 200, w = rng() % 30;
      bs.push_back(Block(a, a + w));
    }
    std::sort(bs.begin(), bs.end(), [](const leaper::BlockRef& x, const leaper::BlockRef& y) {
      return x.first_range < y.first_range;
    });
    std::vector<leaper::RangeId> hot;
    for (int i = 0; i < n_hot; ++i) hot.push_back(rng() % 230);
    std::sort(hot.begin(), hot.end());
    hot.erase(std::unique(hot.begin(), hot.end()), hot.end());
    const std::vector<leaper::RangeSpan> sp = leaper::ToSpans(hot);
    std::vector<size_t> got = leaper::SelectOverlapping(bs, sp);
    std::sort(got.begin(), got.end());
    got.erase(std::unique(got.begin(), got.end()), got.end());
    std::vector<size_t> want;
    for (size_t i = 0; i < bs.size(); ++i) {
      if (leaper::BlockOverlaps(bs[i], sp)) want.push_back(i);
    }
    if (got != want) {
      CHECK(false, "SelectOverlapping disagrees with per-block BlockOverlaps");
      return;
    }
  }
}

// ---------------------------------------------------------------------------
void TestSingleModelCompactionPrefetch(const std::string& model) {
  RecordingCache cache;
  std::unique_ptr<leaper::RangeMapper> mapper = leaper::NewDecimalRangeMapper(1000);
  std::unique_ptr<leaper::Leaper> core = OpenCore(leaper::Policy::kLeaper, model,
                                                  mapper.get(), &cache);
  leaper::CompactionInfo info;
  info.is_flush = false;
  info.est_blocks = 3;
  core->OnCompactionBegin(info, {Block(0, 9), Block(10, 19), Block(20, 29)}, 1000000);
  // Prefetch phase steps are k1+1..k1+k2 = 3..5; one always-hot model must
  // cover them (it used to be clamped to step 1 and predict nothing).
  CHECK(core->ShouldPrefetch(Block(5, 5), 1000000),
        "single model: compaction output in a candidate range not prefetched");
  core->OnCompactionEnd(info, 2000000);
}

// ---------------------------------------------------------------------------
void TestFlushHasCandidates(const std::string& model) {
  RecordingCache cache;
  std::unique_ptr<leaper::RangeMapper> mapper = leaper::NewDecimalRangeMapper(1000);
  std::unique_ptr<leaper::Leaper> core = OpenCore(leaper::Policy::kLeaper, model,
                                                  mapper.get(), &cache);
  leaper::CompactionInfo info;
  info.is_flush = true;
  // The engine reports the memtable's key span as one size-0 pseudo-block.
  core->OnCompactionBegin(info, {Block(40, 60, 0)}, 1000000);
  CHECK(core->ShouldPrefetch(Block(50, 50), 1000000),
        "flush: output block inside the memtable span not prefetched");
  CHECK(!core->ShouldPrefetch(Block(90, 90), 1000000),
        "flush: output block outside the memtable span was prefetched");
  core->OnCompactionEnd(info, 2000000);

  // And the old behaviour, an empty input list, must still be safe: nothing
  // to predict over, nothing prefetched.
  core->OnCompactionBegin(info, {}, 3000000);
  CHECK(!core->ShouldPrefetch(Block(50, 50), 3000000),
        "flush with no candidates prefetched something");
  core->OnCompactionEnd(info, 4000000);
}

// ---------------------------------------------------------------------------
void TestNestedFlushRestoresCompaction(const std::string& model) {
  // (a) Learned policy: the compaction's hot set must survive a nested flush.
  {
    RecordingCache cache;
    std::unique_ptr<leaper::RangeMapper> mapper = leaper::NewDecimalRangeMapper(1000);
    std::unique_ptr<leaper::Leaper> core = OpenCore(leaper::Policy::kLeaper, model,
                                                    mapper.get(), &cache);
    leaper::CompactionInfo comp;
    comp.is_flush = false;
    comp.est_blocks = 1;
    core->OnCompactionBegin(comp, {Block(0, 9)}, 1000000);
    CHECK(core->ShouldPrefetch(Block(3, 3), 1000000), "outer compaction: block 3 hot before flush");

    leaper::CompactionInfo flush;
    flush.is_flush = true;
    core->OnCompactionBegin(flush, {Block(500, 500, 0)}, 1100000);
    CHECK(core->ShouldPrefetch(Block(500, 500), 1100000), "inner flush: its own block hot");
    CHECK(!core->ShouldPrefetch(Block(3, 3), 1100000), "inner flush: outer block not in flush set");
    core->OnCompactionEnd(flush, 1200000);

    CHECK(core->ShouldPrefetch(Block(3, 3), 1300000),
          "outer compaction: block 3 no longer hot after the nested flush ended");
    CHECK(!core->ShouldPrefetch(Block(500, 500), 1300000),
          "outer compaction: the flush's hot set leaked into the compaction");
    core->OnCompactionEnd(comp, 1400000);
  }
  // (b) Flush-only warming: the flush flag must be restored too.
  {
    RecordingCache cache;
    std::unique_ptr<leaper::RangeMapper> mapper = leaper::NewDecimalRangeMapper(1000);
    std::unique_ptr<leaper::Leaper> core = OpenCore(leaper::Policy::kWarmFlush, "",
                                                    mapper.get(), &cache);
    leaper::CompactionInfo comp;
    comp.is_flush = false;
    core->OnCompactionBegin(comp, {Block(0, 9)}, 1000000);
    CHECK(!core->ShouldPrefetch(Block(3, 3), 1000000), "warm_flush: compaction output not warmed");
    leaper::CompactionInfo flush;
    flush.is_flush = true;
    core->OnCompactionBegin(flush, {Block(500, 500, 0)}, 1100000);
    CHECK(core->ShouldPrefetch(Block(500, 500), 1100000), "warm_flush: flush output warmed");
    core->OnCompactionEnd(flush, 1200000);
    CHECK(!core->ShouldPrefetch(Block(4, 4), 1300000),
          "warm_flush: compaction output warmed after a nested flush (flag leaked)");
    core->OnCompactionEnd(comp, 1400000);
  }
  // (c) The prefetch budget must be the outer job's again after the flush.
  {
    RecordingCache cache;
    std::unique_ptr<leaper::RangeMapper> mapper = leaper::NewDecimalRangeMapper(1000);
    leaper::Options o;
    o.policy = leaper::Policy::kWarmAll;
    o.range_size = 1000;
    o.max_range_id = 1000;
    o.cache_bytes = 10.0 * 4096;  // budget: ten blocks per job
    o.max_prefetch_frac = 1.0;
    std::string err;
    std::unique_ptr<leaper::Leaper> core = leaper::Leaper::Open(o, mapper.get(), &cache, &err);
    leaper::CompactionInfo comp;
    comp.is_flush = false;
    core->OnCompactionBegin(comp, {Block(0, 9)}, 1000000);
    for (int i = 0; i < 8; ++i) CHECK(core->ShouldPrefetch(Block(i, i), 1000000), "budget: first 8");
    leaper::CompactionInfo flush;
    flush.is_flush = true;
    core->OnCompactionBegin(flush, {}, 1100000);
    for (int i = 0; i < 10; ++i) core->ShouldPrefetch(Block(100 + i, 100 + i), 1100000);
    core->OnCompactionEnd(flush, 1200000);
    CHECK(core->ShouldPrefetch(Block(8, 8), 1300000), "budget: 9th block of the compaction");
    CHECK(core->ShouldPrefetch(Block(9, 9), 1300000), "budget: 10th block of the compaction");
    CHECK(!core->ShouldPrefetch(Block(10, 10), 1300000), "budget: 11th block must be refused");
    core->OnCompactionEnd(comp, 1400000);
  }
}

}  // namespace

int main(int argc, char** argv) {
  const std::string dir = argc > 1 ? argv[1] : "/tmp";
  const std::string model = WriteAlwaysHotModel(dir, 2 * 6 + 3 + 3);
  TestOverlapNested();
  TestSingleModelCompactionPrefetch(model);
  TestFlushHasCandidates(model);
  TestNestedFlushRestoresCompaction(model);
  if (g_failed) {
    std::fprintf(stderr, "core_check: %d FAILED\n", g_failed);
    return 1;
  }
  std::printf("core_check: PASS\n");
  return 0;
}
