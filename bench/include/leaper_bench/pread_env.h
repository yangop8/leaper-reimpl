// Copyright (c) 2026 The Leaper Authors. BSD-3-Clause (see LICENSE).
//
// PreadEnv -- an Env that forces LevelDB down the pread path, and optionally
// out of the OS page cache.
//
// WHY THIS IS REQUIRED, NOT AN OPTIMIZATION:
//
// On 64-bit POSIX, LevelDB mmaps the first g_mmap_limit = 1000 SST files
// (util/env_posix.cc:46). ReadBlock then sees that the file handed back a
// pointer into memory it owns and marks the block non-cachable to avoid
// double-caching (table/format.cc:99-106). Table::BlockReader therefore never
// calls block_cache->Insert(), and the block cache stays empty: measured on
// stock LevelDB 1.23, 2.07M block cache lookups produced 0 hits and 0 inserts.
// Any study of the block cache -- including the cache invalidation problem
// Leaper addresses -- has to take LevelDB off the mmap path first, otherwise
// the caching is really being done by the OS page cache.
//
// RocksDB defaults to allow_mmap_reads=false, so this also makes the two
// engines comparable, which matters for the LevelDB -> RocksDB port.
//
// |bypass_page_cache| additionally asks the kernel not to retain these pages,
// so a block cache miss costs a real device read. Without it, a DB that fits
// in RAM shows the hit-ratio drop but not the latency spike, because every
// miss is served from the page cache.
//
// |read_delay_us| emulates slow storage. The paper's 10x latency spikes and
// long hit-ratio recovery times come from spinning disks, where a block cache
// miss costs milliseconds; on NVMe a miss costs tens of microseconds and the
// latency amplifier is simply absent, so the tail barely moves even when the
// hit ratio drops. Adding a fixed delay to every device read reproduces that
// regime deterministically, which is more reproducible than sourcing an actual
// disk. It applies to compaction reads too, exactly as a real slow device
// would.

#ifndef LEAPER_BENCH_PREAD_ENV_H_
#define LEAPER_BENCH_PREAD_ENV_H_

#include <fcntl.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>

#include "leveldb/env.h"
#include "leveldb/status.h"

namespace leaper_bench {

inline std::atomic<bool>& ReadDelayEnabled() {
  static std::atomic<bool> enabled{false};
  return enabled;
}

inline leveldb::Status PosixError(const std::string& context, int err) {
  if (err == ENOENT) return leveldb::Status::NotFound(context, std::strerror(err));
  return leveldb::Status::IOError(context, std::strerror(err));
}

// The emulated delay is gated at runtime so it applies to the measured run but
// not to the database load. Loading 20M records means compacting gigabytes, and
// at 200 us per 4 KiB read that setup would take longer than the experiment.
class PreadRandomAccessFile : public leveldb::RandomAccessFile {
 public:
  PreadRandomAccessFile(std::string filename, int fd, bool bypass_page_cache,
                        int read_delay_us)
      : filename_(std::move(filename)), fd_(fd), bypass_(bypass_page_cache),
        delay_us_(read_delay_us) {}

  ~PreadRandomAccessFile() override { ::close(fd_); }

  leveldb::Status Read(uint64_t offset, size_t n, leveldb::Slice* result,
                       char* scratch) const override {
    const ssize_t got = ::pread(fd_, scratch, n, static_cast<off_t>(offset));
    if (got < 0) {
      *result = leveldb::Slice();
      return PosixError(filename_, errno);
    }
    *result = leveldb::Slice(scratch, static_cast<size_t>(got));
    if (delay_us_ > 0 && ReadDelayEnabled().load(std::memory_order_relaxed)) {
      std::this_thread::sleep_for(std::chrono::microseconds(delay_us_));
    }
#if defined(__linux__)
    if (bypass_) {
      ::posix_fadvise(fd_, static_cast<off_t>(offset), static_cast<off_t>(n),
                      POSIX_FADV_DONTNEED);
    }
#endif
    return leveldb::Status::OK();
  }

 private:
  const std::string filename_;
  const int fd_;
  const bool bypass_;
  const int delay_us_;
};

class PreadEnv : public leveldb::EnvWrapper {
 public:
  PreadEnv(leveldb::Env* base, bool bypass_page_cache, int read_delay_us)
      : leveldb::EnvWrapper(base), bypass_(bypass_page_cache),
        delay_us_(read_delay_us) {}

  leveldb::Status NewRandomAccessFile(const std::string& filename,
                                      leveldb::RandomAccessFile** result) override {
    *result = nullptr;
    const int fd = ::open(filename.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return PosixError(filename, errno);
#if defined(__APPLE__)
    if (bypass_) ::fcntl(fd, F_NOCACHE, 1);
#elif defined(__linux__)
    if (bypass_) ::posix_fadvise(fd, 0, 0, POSIX_FADV_RANDOM);
#endif
    *result = new PreadRandomAccessFile(filename, fd, bypass_, delay_us_);
    return leveldb::Status::OK();
  }

 private:
  const bool bypass_;
  const int delay_us_;
};

}  // namespace leaper_bench

#endif  // LEAPER_BENCH_PREAD_ENV_H_
