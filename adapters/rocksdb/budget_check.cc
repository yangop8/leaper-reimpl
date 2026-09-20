// Copyright (c) 2026 The Leaper Authors. BSD-3-Clause (see LICENSE).
//
// budget_check: the RocksDB adapter's per-job warm budget must hold on a
// file in which the predicted-hot ranges have no keys.
//
// Review round 3 (docs/code-review-round3-2026-09-20.md): prediction is over
// the whole key space but warming walks one output file at a time, so a
// predicted range often has no keys in the file being warmed. A Seek to such
// a range still reads the block it lands in, and a budget checked only per
// key inside the scan loop never sees those reads: on a file whose keys sit
// in the odd 1,000-key ranges only, choosing the even ranges read 355 blocks
// past a budget of 32. The check now brackets every Seek as well.
//
// Setup mirrors the review's: 12,000 records at key (2i+1)*1000, 100-byte
// values, no compression, 4 KiB blocks, one flushed SST, a 64 MiB block
// cache so eviction cannot mask whether the scan stopped, and a fixed oracle
// that selects the even ranges. The adapter is driven through its public
// EventListener callbacks, as RocksDB would.
#include <cinttypes>
#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "leaper_rocksdb.h"
#include "rocksdb/advanced_cache.h"
#include "rocksdb/cache.h"
#include "rocksdb/db.h"
#include "rocksdb/env.h"
#include "rocksdb/listener.h"
#include "rocksdb/metadata.h"
#include "rocksdb/options.h"
#include "rocksdb/table.h"

static std::string Key(uint64_t i) {
  char b[32];
  std::snprintf(b, sizeof b, "%016" PRIu64, i);
  return b;
}

static int g_failed = 0;

// Warm |path| once through a fresh adapter that selects |ranges| for every
// slot, with |budget_blocks| (0 = unlimited). Returns blocks the adapter
// counted.
static uint64_t WarmOnce(rocksdb::DB* db, const rocksdb::Options& dbopts,
                         const std::string& path, const std::string& dir,
                         const std::vector<uint64_t>& ranges, uint64_t budget_blocks,
                         const char* label) {
  const std::string oracle = dir + "/oracle_" + label + ".txt";
  {
    std::ofstream out(oracle);
    for (int slot = 0; slot <= 30; ++slot) {
      out << slot;
      for (uint64_t r : ranges) out << ' ' << r;
      out << '\n';
    }
  }
  leaper_rocksdb::AdapterOptions ao;
  ao.core.policy = leaper::Policy::kOracle;
  ao.core.oracle_path = oracle;
  ao.core.range_size = 1000;
  ao.core.max_range_id = 24000;
  ao.core.slot_seconds = 1.0;
  ao.core.cache_bytes = budget_blocks ? 4096.0 * budget_blocks : 4096.0;
  ao.core.max_prefetch_frac = budget_blocks ? 1.0 : 0.0;  // 0 -> adapter budget 0 = unlimited
  ao.warm_mode = "sst";
  ao.num_ranges = 24000;
  std::string err;
  std::unique_ptr<leaper_rocksdb::Adapter> a = leaper_rocksdb::Adapter::Create(ao, &err);
  if (a == nullptr) {
    std::fprintf(stderr, "Adapter::Create: %s\n", err.c_str());
    std::exit(2);
  }
  a->SetTableFactory(dbopts.table_factory, dbopts.comparator);
  a->SetDB(db);
  std::shared_ptr<rocksdb::EventListener> l = a->listener();

  rocksdb::FlushJobInfo fi;
  fi.job_id = 1;
  fi.file_path = path;
  l->OnFlushBegin(db, fi);
  l->OnFlushCompleted(db, fi);
  std::printf("  %-28s budget=%3" PRIu64 "  blocks read=%5" PRIu64 "  files=%" PRIu64
              "  open_failed=%" PRIu64 "  budget_stops=%" PRIu64 "\n",
              label, budget_blocks, a->warmed_blocks(), a->warm_files(),
              a->warm_open_failed(), a->warm_budget_stops());
  return a->warmed_blocks();
}

int main(int argc, char** argv) {
  const std::string dir = argc > 1 ? argv[1] : "/tmp/budget_check";
  const std::string dbdir = dir + "/db";
  rocksdb::Env::Default()->CreateDirIfMissing(dir);

  rocksdb::Options opts;
  opts.create_if_missing = true;
  opts.compression = rocksdb::kNoCompression;
  opts.write_buffer_size = 64 << 20;
  rocksdb::BlockBasedTableOptions bbt;
  bbt.block_cache = rocksdb::NewLRUCache(64 << 20);
  bbt.block_size = 4096;
  opts.table_factory.reset(rocksdb::NewBlockBasedTableFactory(bbt));
  rocksdb::DestroyDB(dbdir, opts);
  std::unique_ptr<rocksdb::DB> db;
  rocksdb::Status s = rocksdb::DB::Open(opts, dbdir, &db);
  if (!s.ok()) { std::fprintf(stderr, "open: %s\n", s.ToString().c_str()); return 2; }

  // Keys only in the odd ranges: key (2i+1)*1000 lies in range 2i+1.
  const std::string value(100, 'v');
  for (uint64_t i = 0; i < 12000; ++i) db->Put(rocksdb::WriteOptions(), Key((2 * i + 1) * 1000), value);
  db->Flush(rocksdb::FlushOptions());
  std::vector<rocksdb::LiveFileMetaData> files;
  db->GetLiveFilesMetaData(&files);
  if (files.size() != 1) { std::fprintf(stderr, "expected one SST, got %zu\n", files.size()); return 2; }
  const std::string path = files[0].db_path + "/" + files[0].name;
  std::printf("SST %s, %" PRIu64 " bytes, keys in odd ranges only\n", files[0].name.c_str(), files[0].size);

  std::vector<uint64_t> even, odd;
  for (uint64_t r = 0; r < 24000; r += 2) { even.push_back(r); odd.push_back(r + 1); }

  auto fresh = [&] { bbt.block_cache->EraseUnRefEntries(); };

  fresh();
  const uint64_t unlimited = WarmOnce(db.get(), opts, path, dir, even, 0, "even ranges, no budget");
  fresh();
  const uint64_t even32 = WarmOnce(db.get(), opts, path, dir, even, 32, "even ranges (empty here)");
  fresh();
  const uint64_t odd32 = WarmOnce(db.get(), opts, path, dir, odd, 32, "odd ranges (have keys)");

  // The unlimited run has to read a lot, or the budgeted runs prove nothing.
  if (unlimited < 100) {
    std::fprintf(stderr, "FAIL: unlimited warm read only %" PRIu64 " blocks; test cannot detect a bypass\n", unlimited);
    ++g_failed;
  }
  // One indivisible read past the budget is allowed; a range's worth is not.
  if (even32 > 33) {
    std::fprintf(stderr, "FAIL: empty predicted ranges read %" PRIu64 " blocks past a budget of 32\n", even32);
    ++g_failed;
  }
  if (odd32 > 33) {
    std::fprintf(stderr, "FAIL: populated ranges read %" PRIu64 " blocks past a budget of 32\n", odd32);
    ++g_failed;
  }
  db.reset();
  rocksdb::DestroyDB(dbdir, opts);
  if (g_failed) { std::fprintf(stderr, "budget_check: FAILED\n"); return 1; }
  std::printf("budget_check: PASS\n");
  return 0;
}
