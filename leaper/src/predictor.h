// Copyright (c) 2026 The Leaper Authors. BSD-3-Clause (see LICENSE).
//
// Multi-step prediction (paper Section 6.1) and the 18-feature vector
// (Section 4.2). One model per step k predicts "read in the k-th interval from
// now"; the two-phase prefetcher unions steps 1..k1 for T1 and k1+1..k1+k2 for
// T2. Feature order must match tools/train_leaper.py exactly:
//
//   [0 .. h-1]      read arrival rate, oldest slot first  (read_rate_t-h .. t-1)
//   [h .. 2h-1]     write arrival rate, same order
//   [2h .. 2h+2]    hour, minute, second of the prediction time
//   [2h+3 .. +gamma] precursor read rates in the previous slot

#ifndef LEAPER_PREDICTOR_H_
#define LEAPER_PREDICTOR_H_

#include <string>
#include <unordered_map>
#include <vector>

#include "collector.h"
#include "gbdt.h"
#include "leaper/leaper.h"

namespace leaper {

class Predictor {
 public:
  bool Load(const std::vector<std::string>& model_paths,
            const std::string& precursor_path, int history, int gamma,
            std::string* error);

  int num_steps() const { return static_cast<int>(models_.size()); }
  int feature_count() const { return 2 * history_ + 3 + gamma_; }

  // Fills |out| with the ranges among |candidates| predicted hot for any step
  // in [step_lo, step_hi] (1-based, clamped to the models available).
  void PredictHot(const Collector& collector, const std::vector<RangeId>& candidates,
                  int step_lo, int step_hi, uint64_t now_us, double threshold,
                  std::vector<RangeId>* out, uint64_t* inferences) const;

 private:
  void BuildFeatures(const Collector& collector, RangeId range, uint64_t now_us,
                     float* f) const;

  std::vector<GbdtModel> models_;
  std::unordered_map<RangeId, std::vector<RangeId>> precursors_;
  int history_ = 6;
  int gamma_ = 3;
};

}  // namespace leaper

#endif  // LEAPER_PREDICTOR_H_
