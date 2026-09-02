// Copyright (c) 2026 The Leaper Authors. BSD-3-Clause (see LICENSE).
//
// Access trace capture for Leaper's offline pipeline (M1).
//
// The offline learner needs the raw (time, key, op) stream so that it can run
// key range selection (Algorithm 1) at the finest granularity A and only then
// decide the range size; aggregating in the storage engine would prejudge that
// choice. Records are 8 bytes and written per thread with no shared state, so
// tracing does not serialise the workload it is measuring.
//
// Record layout (little-endian, packed, 8 bytes):
//   uint32 t_ms     milliseconds since the measurement window opened
//   uint32 key_op   (key_index << 2) | op     -- key_index < 2^30; op 3 = scan (seek key)
//
// A sidecar "<prefix>.meta" records the run parameters the analysis needs.

#ifndef LEAPER_BENCH_TRACE_H_
#define LEAPER_BENCH_TRACE_H_

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace leaper_bench {

enum OpType : uint8_t { kOpRead = 0, kOpUpdate = 1, kOpInsert = 2, kOpScan = 3 };

class TraceWriter {
 public:
  explicit TraceWriter(const std::string& path) {
    file_ = std::fopen(path.c_str(), "wb");
    if (file_ != nullptr) std::setvbuf(file_, nullptr, _IOFBF, 1 << 20);
    buf_.reserve(kBatch * 2);
  }

  ~TraceWriter() { Close(); }

  bool ok() const { return file_ != nullptr; }

  void Add(uint32_t t_ms, uint64_t key_index, OpType op) {
    if (file_ == nullptr) return;
    buf_.push_back(t_ms);
    buf_.push_back(static_cast<uint32_t>((key_index << 2) | op));
    if (buf_.size() >= kBatch * 2) FlushBuffer();
  }

  void Close() {
    if (file_ == nullptr) return;
    FlushBuffer();
    std::fclose(file_);
    file_ = nullptr;
  }

 private:
  static constexpr size_t kBatch = 8192;

  void FlushBuffer() {
    if (!buf_.empty()) {
      std::fwrite(buf_.data(), sizeof(uint32_t), buf_.size(), file_);
      buf_.clear();
    }
  }

  std::FILE* file_ = nullptr;
  std::vector<uint32_t> buf_;
};

}  // namespace leaper_bench

#endif  // LEAPER_BENCH_TRACE_H_
