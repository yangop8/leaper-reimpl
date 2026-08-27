// Copyright (c) 2026 The Leaper Authors. BSD-3-Clause (see LICENSE).
//
// Cross-implementation check: our hand-written LightGBM text-model scorer must
// reproduce LightGBM's own predictions. A silently wrong scorer would make
// every online number meaningless while still looking plausible, so this runs
// as a build-time test, not as an optional tool.
//
//   gbdt_check <model.txt> <eval.csv>
//
// eval.csv is produced by tools/train_leaper.py --dump_eval: one header row of
// feature names plus lgb_score, then rows of feature values and the score
// LightGBM computed for them.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "gbdt.h"

int main(int argc, char** argv) {
  if (argc != 3) {
    std::fprintf(stderr, "usage: %s <model.txt> <eval.csv>\n", argv[0]);
    return 2;
  }
  leaper::GbdtModel model;
  std::string err;
  if (!model.LoadFile(argv[1], &err)) {
    std::fprintf(stderr, "load failed: %s\n", err.c_str());
    return 1;
  }

  std::ifstream in(argv[2]);
  if (!in) {
    std::fprintf(stderr, "cannot open %s\n", argv[2]);
    return 1;
  }
  std::string line;
  if (!std::getline(in, line)) return 1;  // header

  size_t n = 0;
  double max_abs = 0.0, sum_abs = 0.0;
  std::vector<float> f;
  while (std::getline(in, line)) {
    f.clear();
    std::istringstream ls(line);
    std::string tok;
    while (std::getline(ls, tok, ',')) f.push_back(std::atof(tok.c_str()));
    if (f.size() < 2) continue;
    const double expected = f.back();
    f.pop_back();
    const double got = model.Predict(f.data(), static_cast<int>(f.size()));
    const double d = std::fabs(got - expected);
    max_abs = d > max_abs ? d : max_abs;
    sum_abs += d;
    ++n;
  }
  if (n == 0) {
    std::fprintf(stderr, "no rows in %s\n", argv[2]);
    return 1;
  }
  std::printf("gbdt_check: %zu rows, %zu trees, mean|diff|=%.3e max|diff|=%.3e\n",
              n, model.num_trees(), sum_abs / n, max_abs);
  // float32 features and a double accumulator over hundreds of trees; 1e-6 is
  // comfortably above representation noise and far below any decision boundary.
  if (max_abs > 1e-6) {
    std::fprintf(stderr, "FAIL: scorer disagrees with LightGBM\n");
    return 1;
  }
  std::printf("gbdt_check: PASS\n");
  return 0;
}
