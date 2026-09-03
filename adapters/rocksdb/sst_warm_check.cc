// Copyright (c) 2026 The Leaper Authors. BSD-3-Clause (see LICENSE).
//
// sst_warm_check: can a plug-in warm a RocksDB block cache at block
// granularity without patching RocksDB?
//
// The block cache key of a data block is derived from the SST's table
// properties (db_session_id, orig_file_number) plus the block offset
// (BlockBasedTable::SetupBaseCacheKey), so an SstFileReader that opens one of
// the DB's own files with the DB's own table factory -- and therefore the
// same block cache -- should insert blocks under exactly the keys the DB's
// reader will look up. This program checks that end to end: warm a key range
// through an SstFileReader, then Get those keys through the DB with
// PerfContext on and require zero block reads from the file.
#include <cinttypes>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "rocksdb/advanced_cache.h"
#include "rocksdb/cache.h"
#include "rocksdb/db.h"
#include "rocksdb/filter_policy.h"
#include "rocksdb/metadata.h"
#include "rocksdb/options.h"
#include "rocksdb/perf_context.h"
#include "rocksdb/perf_level.h"
#include "rocksdb/sst_file_reader.h"
#include "rocksdb/table.h"

static std::string Key(uint64_t i) {
  char b[32];
  std::snprintf(b, sizeof b, "%016" PRIu64, i);
  return b;
}

int main(int argc, char** argv) {
  const std::string dir = argc > 1 ? argv[1] : "/tmp/sst_warm_check_db";
  const uint64_t kKeys = 200000;

  rocksdb::Options opts;
  opts.create_if_missing = true;
  opts.compression = rocksdb::kNoCompression;
  opts.write_buffer_size = 8 << 20;
  rocksdb::BlockBasedTableOptions bbt;
  bbt.block_cache = rocksdb::NewLRUCache(64 << 20);
  bbt.block_size = 4096;
  bbt.filter_policy.reset(rocksdb::NewBloomFilterPolicy(10, false));
  opts.table_factory.reset(rocksdb::NewBlockBasedTableFactory(bbt));
  rocksdb::DestroyDB(dir, opts);

  std::unique_ptr<rocksdb::DB> db;
  rocksdb::Status s = rocksdb::DB::Open(opts, dir, &db);
  if (!s.ok()) { std::fprintf(stderr, "open: %s\n", s.ToString().c_str()); return 2; }

  const std::string value(100, 'v');
  rocksdb::WriteOptions wo;
  for (uint64_t i = 0; i < kKeys; ++i) db->Put(wo, Key(i), value);
  db->Flush(rocksdb::FlushOptions());
  db->CompactRange(rocksdb::CompactRangeOptions(), nullptr, nullptr);

  std::vector<rocksdb::LiveFileMetaData> files;
  db->GetLiveFilesMetaData(&files);
  const std::string probe = Key(50000);
  const rocksdb::LiveFileMetaData* target = nullptr;
  for (const auto& f : files) {
    std::printf("file %s level %d size %" PRIu64 " [%s .. %s]\n", f.name.c_str(), f.level,
                f.size, f.smallestkey.c_str(), f.largestkey.c_str());
    if (f.smallestkey <= probe && probe <= f.largestkey) target = &f;
  }
  if (target == nullptr) { std::fprintf(stderr, "no file holds the probe key\n"); return 2; }

  // Start from an empty cache so every hit below is one the reader put there.
  bbt.block_cache->EraseUnRefEntries();
  const size_t usage0 = bbt.block_cache->GetUsage();

  // Open the DB's own file through an SstFileReader that shares the DB's
  // table factory (and so its block cache), and warm keys [50000, 60000).
  rocksdb::Options ropts;
  ropts.table_factory = opts.table_factory;
  ropts.comparator = opts.comparator;
  rocksdb::SstFileReader reader(ropts);
  s = reader.Open(target->db_path + "/" + target->name);
  if (!s.ok()) { std::fprintf(stderr, "reader open: %s\n", s.ToString().c_str()); return 2; }
  rocksdb::ReadOptions ro;
  ro.fill_cache = true;
  std::unique_ptr<rocksdb::Iterator> it(reader.NewIterator(ro));
  int scanned = 0;
  std::string last;
  for (it->Seek(Key(50000)); it->Valid() && scanned < 10000; it->Next()) {
    ++scanned;
    last = it->key().ToString();
  }
  const size_t usage1 = bbt.block_cache->GetUsage();
  std::printf("reader scanned %d keys (last %s); cache usage %zu -> %zu bytes\n", scanned,
              last.c_str(), usage0, usage1);
  const uint64_t last_k = std::stoull(last);

  // Now read the same keys through the DB, counting block reads per Get.
  rocksdb::SetPerfLevel(rocksdb::PerfLevel::kEnableCount);
  auto get_probe = [&](uint64_t k) {
    rocksdb::get_perf_context()->Reset();
    std::string v;
    rocksdb::Status gs = db->Get(rocksdb::ReadOptions(), Key(k), &v);
    const rocksdb::PerfContext* pc = rocksdb::get_perf_context();
    const uint64_t data_reads = pc->block_read_count - pc->index_block_read_count -
                                pc->filter_block_read_count;
    const uint64_t data_hits = pc->block_cache_hit_count - pc->block_cache_index_hit_count -
                               pc->block_cache_filter_hit_count;
    std::printf("  Get(%s): %s, data block reads from file %" PRIu64 ", cache hits %" PRIu64 "\n",
                Key(k).c_str(), gs.ok() ? "ok" : gs.ToString().c_str(), data_reads, data_hits);
    return std::make_pair(data_reads, data_hits);
  };
  bool pass = true;
  std::printf("warmed region (expect 0 reads, >=1 hit):\n");
  for (uint64_t k : {50000ULL, 52345ULL, 55000ULL, last_k}) {
    auto r = get_probe(k);
    if (r.first != 0 || r.second < 1) pass = false;
  }
  std::printf("control region (expect >=1 read):\n");
  bool control_ok = true;
  for (uint64_t k : {150000ULL, 160000ULL}) {
    auto r = get_probe(k);
    if (r.first < 1) control_ok = false;
  }
  std::printf("%s: SstFileReader %s the DB's block cache keys%s\n",
              pass && control_ok ? "PASS" : "FAIL",
              pass ? "shares" : "does NOT share",
              control_ok ? "" : " (control region unexpectedly cached)");
  db.reset();
  rocksdb::DestroyDB(dir, opts);
  return pass && control_ok ? 0 : 1;
}
