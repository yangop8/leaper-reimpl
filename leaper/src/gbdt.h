// Copyright (c) 2026 The Leaper Authors. BSD-3-Clause (see LICENSE).
//
// Minimal LightGBM text-model scorer.
//
// The paper compiles its model with Treelite for a 3-5x inference speedup. We
// deliberately start without any model-runtime dependency: parsing LightGBM's
// own text dump and walking the trees is ~200 lines, links nothing, and is
// fast enough at this problem size (18 features, a few hundred ranges per
// compaction). Treelite/TL2cgen remains an option for M4 to reproduce the
// paper's speedup claim as an A/B, not a prerequisite for correctness.

#ifndef LEAPER_GBDT_H_
#define LEAPER_GBDT_H_

#include <string>
#include <vector>

namespace leaper {

class GbdtModel {
 public:
  // Parses a LightGBM text model. Returns false and fills |error| on failure.
  bool LoadFile(const std::string& path, std::string* error);
  bool LoadString(const std::string& text, std::string* error);

  // Raw sum of leaf values across trees.
  double RawScore(const float* features, int n) const;
  // Sigmoid of the raw score, matching objective=binary sigmoid:1.
  double Predict(const float* features, int n) const;

  int num_features() const { return max_feature_idx_ + 1; }
  size_t num_trees() const { return trees_.size(); }
  bool ok() const { return !trees_.empty(); }

 private:
  struct Tree {
    std::vector<int> split_feature;
    std::vector<double> threshold;
    std::vector<int> decision_type;
    std::vector<int> left_child;
    std::vector<int> right_child;
    std::vector<double> leaf_value;
  };

  std::vector<Tree> trees_;
  int max_feature_idx_ = -1;
  double sigmoid_ = 1.0;
};

}  // namespace leaper

#endif  // LEAPER_GBDT_H_
