// Copyright (c) 2026 The Leaper Authors. BSD-3-Clause (see LICENSE).
//
// EventLogger: recovers LevelDB's background-operation timeline without
// patching LevelDB, by installing a custom Options::info_log and parsing the
// messages DBImpl already emits:
//
//   db/db_impl.cc:896  "Compacting %d@%d + %d@%d files"        compaction begin
//   db/db_impl.cc:876  "Compacted %d@%d + %d@%d files => ..."  compaction end
//   db/db_impl.cc:513  "Level-0 table #%llu: started"          flush begin
//   db/db_impl.cc:523  "Level-0 table #%llu: %lld bytes ..."   flush end
//   db/db_impl.cc:744  "Moved #%lld to level-%d ..."           trivial move
//
// This is deliberately a read-only observer: M0 must establish the baseline
// phenomenon on stock LevelDB before any Leaper hook exists.

#ifndef LEAPER_BENCH_EVENT_LOGGER_H_
#define LEAPER_BENCH_EVENT_LOGGER_H_

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "leveldb/env.h"

namespace leaper_bench {

enum class EventType { kCompactionBegin, kCompactionEnd, kFlushBegin, kFlushEnd, kTrivialMove };

struct BgEvent {
  double t_secs;      // seconds since benchmark start
  EventType type;
  int level = -1;     // source level for compactions
  long long bytes = 0;
};

inline const char* EventTypeName(EventType t) {
  switch (t) {
    case EventType::kCompactionBegin: return "compaction_begin";
    case EventType::kCompactionEnd:   return "compaction_end";
    case EventType::kFlushBegin:      return "flush_begin";
    case EventType::kFlushEnd:        return "flush_end";
    case EventType::kTrivialMove:     return "trivial_move";
  }
  return "unknown";
}

class EventLogger : public leveldb::Logger {
 public:
  // |start_us| is the benchmark epoch; |sink| may be null to discard the raw log.
  EventLogger(uint64_t start_us, std::FILE* sink) : start_us_(start_us), sink_(sink) {}

  ~EventLogger() override { if (sink_ != nullptr) std::fclose(sink_); }

  void Logv(const char* format, std::va_list ap) override {
    char buf[1024];
    std::vsnprintf(buf, sizeof(buf), format, ap);
    const double t = (NowMicros() - start_us_) / 1e6;

    if (std::strncmp(buf, "Compacting ", 11) == 0) {
      int n0 = 0, l0 = 0, n1 = 0, l1 = 0;
      std::sscanf(buf, "Compacting %d@%d + %d@%d", &n0, &l0, &n1, &l1);
      Record({t, EventType::kCompactionBegin, l0, 0});
      compactions_running_.fetch_add(1, std::memory_order_relaxed);
    } else if (std::strncmp(buf, "Compacted ", 10) == 0) {
      int n0 = 0, l0 = 0, n1 = 0, l1 = 0;
      long long bytes = 0;
      std::sscanf(buf, "Compacted %d@%d + %d@%d files => %lld", &n0, &l0, &n1, &l1, &bytes);
      Record({t, EventType::kCompactionEnd, l0, bytes});
      compactions_running_.fetch_sub(1, std::memory_order_relaxed);
      compactions_done_.fetch_add(1, std::memory_order_relaxed);
      compacted_bytes_.fetch_add(bytes, std::memory_order_relaxed);
    } else if (std::strncmp(buf, "Level-0 table ", 14) == 0) {
      if (std::strstr(buf, ": started") != nullptr) {
        Record({t, EventType::kFlushBegin, 0, 0});
      } else {
        long long bytes = 0;
        const char* p = std::strchr(buf, ':');
        if (p != nullptr) std::sscanf(p, ": %lld bytes", &bytes);
        Record({t, EventType::kFlushEnd, 0, bytes});
        flushes_done_.fetch_add(1, std::memory_order_relaxed);
      }
    } else if (std::strncmp(buf, "Moved #", 7) == 0) {
      int level = -1;
      long long num = 0, bytes = 0;
      std::sscanf(buf, "Moved #%lld to level-%d %lld", &num, &level, &bytes);
      Record({t, EventType::kTrivialMove, level, bytes});
      trivial_moves_.fetch_add(1, std::memory_order_relaxed);
    }

    if (sink_ != nullptr) {
      std::fprintf(sink_, "%10.3f %s\n", t, buf);
      std::fflush(sink_);
    }
  }

  int compactions_running() const { return compactions_running_.load(std::memory_order_relaxed); }
  uint64_t compactions_done() const { return compactions_done_.load(std::memory_order_relaxed); }
  uint64_t flushes_done() const { return flushes_done_.load(std::memory_order_relaxed); }
  uint64_t trivial_moves() const { return trivial_moves_.load(std::memory_order_relaxed); }
  uint64_t compacted_bytes() const { return compacted_bytes_.load(std::memory_order_relaxed); }

  std::vector<BgEvent> events() const {
    std::lock_guard<std::mutex> lock(mu_);
    return events_;
  }

  static uint64_t NowMicros() { return leveldb::Env::Default()->NowMicros(); }

 private:
  void Record(const BgEvent& e) {
    std::lock_guard<std::mutex> lock(mu_);
    events_.push_back(e);
  }

  const uint64_t start_us_;
  std::FILE* sink_;
  mutable std::mutex mu_;
  std::vector<BgEvent> events_;
  std::atomic<int> compactions_running_{0};
  std::atomic<uint64_t> compactions_done_{0}, flushes_done_{0};
  std::atomic<uint64_t> trivial_moves_{0}, compacted_bytes_{0};
};

}  // namespace leaper_bench

#endif  // LEAPER_BENCH_EVENT_LOGGER_H_
