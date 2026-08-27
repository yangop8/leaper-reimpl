// Copyright (c) 2026 The Leaper Authors. BSD-3-Clause (see LICENSE).
//
// Regression test for the key -> range mapping.
//
// LevelDB stores physically shortened keys in its index blocks
// (InternalKeyComparator::FindShortestSeparator, db/dbformat.cc:72-77), so a
// 16-digit key can arrive as a 14-byte prefix with its last byte incremented.
// Parsing that as an integer instead of restoring the width made every
// index-derived block bound collapse towards range 0. That bug cost Leaper
// 2.4 percentage points of hit ratio against a policy that merely evicts dead
// blocks, and it was invisible in every unit-free smoke test.

#include <cstdio>
#include <string>

#include "leaper/leaper.h"
#include "overlap.h"

namespace {

int failures = 0;

void Expect(bool ok, const std::string& what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    ++failures;
  }
}

void ExpectEq(uint64_t got, uint64_t want, const std::string& what) {
  if (got != want) {
    std::fprintf(stderr, "FAIL: %s: got %llu want %llu\n", what.c_str(),
                 static_cast<unsigned long long>(got),
                 static_cast<unsigned long long>(want));
    ++failures;
  }
}

std::string Key(unsigned long long v) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%016llu", v);
  return std::string(buf, 16);
}

}  // namespace

int main() {
  auto m = leaper::NewDecimalRangeMapper(40000);

  // Full-width keys.
  ExpectEq(m->Map(Key(0).data(), 16), 0, "key 0");
  ExpectEq(m->Map(Key(39999).data(), 16), 0, "key 39999");
  ExpectEq(m->Map(Key(40000).data(), 16), 1, "key 40000");
  ExpectEq(m->Map(Key(123456).data(), 16), 3, "key 123456");

  // Shortened separators, as they appear in a LevelDB index block. The
  // 14-character prefix of 0000000000123456 must map as 123500, not 1235.
  const std::string trunc = Key(123456).substr(0, 14);
  ExpectEq(m->Map(trunc.data(), trunc.size()), 123500 / 40000, "shortened key");
  Expect(m->Map(trunc.data(), trunc.size()) != 1235 / 40000,
         "shortened key must not parse as a small integer");

  // Round trip through RangeStartKey.
  for (uint64_t r : {0ull, 1ull, 7ull, 250ull}) {
    const std::string k = m->RangeStartKey(r);
    ExpectEq(k.size(), 16, "RangeStartKey width");
    ExpectEq(m->Map(k.data(), k.size()), r, "RangeStartKey round trip");
  }

  // Monotonicity, which Algorithm 3's binary search depends on.
  uint64_t prev = 0;
  for (unsigned long long v = 0; v < 4000000ull; v += 7919) {
    const std::string k = Key(v);
    const uint64_t r = m->Map(k.data(), 16);
    Expect(r >= prev, "mapping must be monotone");
    prev = r;
  }

  // Overlap check: a block spanning ranges [3,5] overlaps a hot span [5,9].
  leaper::BlockRef b;
  b.first_range = 3;
  b.last_range = 5;
  std::vector<leaper::RangeId> hot = {5, 6, 7, 9};
  const std::vector<leaper::RangeSpan> spans = leaper::ToSpans(hot);
  ExpectEq(spans.size(), 2, "ToSpans merges 5,6,7 and keeps 9");
  Expect(leaper::BlockOverlaps(b, spans), "block [3,5] overlaps span [5,7]");
  b.first_range = 10;
  b.last_range = 12;
  Expect(!leaper::BlockOverlaps(b, spans), "block [10,12] overlaps nothing");

  // LevelDB's last index entry per SST comes from FindShortSuccessor, which
  // increments the first byte and truncates: 0000000000123456 becomes "1".
  // Restored to width that is 10^15 -- a range id of 25 billion. Expanding
  // such a span into individual ids allocated until the process was killed.
  const std::string successor = "1";
  const uint64_t huge = m->Map(successor.data(), successor.size());
  Expect(huge > 1000000ull,
         "short successor really does map to an out-of-space range id");
  std::fprintf(stderr, "  (short successor \"1\" maps to range %llu; "
               "callers must clamp)\n", static_cast<unsigned long long>(huge));

  if (failures == 0) std::printf("mapper_check: PASS\n");
  return failures == 0 ? 0 : 1;
}
