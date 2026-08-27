// Copyright (c) 2026 The Leaper Authors. BSD-3-Clause (see LICENSE).
//
// Key distributions for the Leaper benchmark.
//
// NOTE on scrambling: YCSB's ScrambledZipfian deliberately spreads the hot
// ranks uniformly over the key space. That is exactly wrong for this study:
// Leaper predicts at *key range* granularity, so the benchmark must preserve
// the property real workloads have -- hot keys are clustered in contiguous
// regions of the key space (e.g. recent rows under an auto-increment primary
// key). We therefore map zipfian rank r to key (hotspot + r) mod N, keeping
// the hot set contiguous. Use --key_dist=scrambled only as an ablation that
// destroys range locality.

#ifndef LEAPER_BENCH_KEYGEN_H_
#define LEAPER_BENCH_KEYGEN_H_

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>

namespace leaper_bench {

// YCSB-compatible zipfian rank generator (Gray et al., SIGMOD'94).
class ZipfianRank {
 public:
  ZipfianRank(uint64_t n, double theta) : n_(n), theta_(theta) {
    zetan_ = Zeta(n, theta);
    zeta2_ = Zeta(2, theta);
    alpha_ = 1.0 / (1.0 - theta);
    eta_ = (1.0 - std::pow(2.0 / static_cast<double>(n), 1.0 - theta)) /
           (1.0 - zeta2_ / zetan_);
  }

  uint64_t Next(std::mt19937_64* rng) const {
    const double u = std::generate_canonical<double, 53>(*rng);
    const double uz = u * zetan_;
    if (uz < 1.0) return 0;
    if (uz < 1.0 + std::pow(0.5, theta_)) return 1;
    const double v = eta_ * u - eta_ + 1.0;
    uint64_t r = static_cast<uint64_t>(static_cast<double>(n_) * std::pow(v, alpha_));
    return r < n_ ? r : n_ - 1;
  }

 private:
  static double Zeta(uint64_t n, double theta) {
    double sum = 0.0;
    for (uint64_t i = 1; i <= n; ++i) sum += 1.0 / std::pow(static_cast<double>(i), theta);
    return sum;
  }

  uint64_t n_;
  double theta_, zetan_, zeta2_, alpha_, eta_;
};

enum class KeyDist { kZipfContiguous, kScrambled, kUniform, kLifecycle };

// Workload dynamics.
//
// A stationary zipfian stream is useless for evaluating Leaper's model: every
// key range's arrival rate is constant, so the trivial predictor "hot in the
// last slot => hot in the next slot" is already optimal and there is nothing
// for a learned model to add. The paper's workloads are not stationary -- an
// e-commerce hot set slides forward as new orders arrive under an
// auto-increment primary key, and access volume is periodic over a day. Both
// are reproduced here:
//
//   drift   the hot region advances by |shift| keys per second, so at the
//           leading edge a range that was cold becomes hot and vice versa;
//           the naive predictor is systematically late at the frontier
//   phases  the hot region jumps between |phases| disjoint regions every
//           |phase_period_s| seconds, which is what makes the paper's
//           timestamp features carry signal at all
struct Dynamics {
  double shift_per_s = 0.0;   // keys/second the hotspot advances
  int phases = 0;             // 0 or 1 disables phase rotation
  double phase_period_s = 0.0;
};

// Lifecycle workload.
//
// Smooth zipfian streams -- with or without a drifting hotspot -- cannot
// evaluate Leaper's model. Measured on a 500k-key zipf-0.99 trace, the paper's
// own baseline ("read in the last interval => read in the next") scores
// precision = recall = AUC = 1.0000: every range that was hot stays hot, so a
// learned model can at best tie. The paper's baseline scores 0.83 recall,
// meaning roughly 17% of the ranges hot in one interval were not hot in the
// previous one. That churn is the thing being predicted.
//
// Pure random churn would not help either: a memoryless hot set is
// unpredictable for any model. What makes the paper's features work is that
// churn has *shape*. Two shapes are generated here, one per feature family:
//
//   lifecycle  a range ramps up, plateaus and decays over |lifetime_s|. Six
//              intervals of arrival-rate history say where a range sits on
//              that arc; the last interval alone does not. This is what lets a
//              model predict deaths, which the naive rule always gets wrong.
//
//   chains     within one lifetime a hot slot activates |chain| ranges
//              A, f(A), f(f(A)) ... each lagged by |chain_lag| of the lifetime,
//              so they are hot *together* with one leading the others. This is
//              the paper's precursor -- "the probability of buying a piano rack
//              rises after buying a piano" -- and note it has to be an
//              overlapping lead, not a hand-off: Algorithm 2 keeps a candidate
//              only if the cosine similarity of the two arrival-rate vectors
//              exceeds a threshold, so ranges hot in disjoint windows are
//              rejected no matter how reliably one follows the other.
//              chain = 1 is the control, where precursor features must
//              contribute nothing.
//
// Occupancy and phase are pure functions of (seed, slot, time), so every worker
// thread agrees with no shared state and no locking.
//
// |cold_frac| defaults to 0 on purpose. Uniform background traffic destroys the
// label: at 40k ops/s over 1000 ranges even 5% cold traffic gives every range
// several accesses per interval, so "accessed at least once in the next
// interval" is true everywhere and there is nothing to predict.
struct Lifecycle {
  uint64_t range_size = 10000;  // keys per generated range
  int hot_slots = 64;           // concurrently active chains
  double lifetime_s = 60.0;
  double ramp_frac = 0.25;      // share of a member's window spent ramping each way
  double cold_frac = 0.0;       // reads sent uniformly over the whole key space
  int chain = 1;                // ranges per chain
  double chain_lag = 0.2;       // lag between chain members, as a share of lifetime
  uint64_t seed = 12345;
};

class LifecycleChooser {
 public:
  LifecycleChooser(uint64_t n, Lifecycle cfg) : n_(n), cfg_(cfg) {
    n_ranges_ = n / cfg_.range_size;
    if (n_ranges_ == 0) n_ranges_ = 1;
    members_ = cfg_.hot_slots * (cfg_.chain > 1 ? cfg_.chain : 1);
  }

  uint64_t Next(std::mt19937_64* rng, double t) const {
    std::uniform_real_distribution<double> u01(0.0, 1.0);
    if (cfg_.cold_frac > 0.0 && u01(*rng) < cfg_.cold_frac) {
      return std::uniform_int_distribution<uint64_t>(0, n_ - 1)(*rng);
    }
    // Rejection sampling over all chain members: pick one uniformly, accept
    // with probability equal to its current weight.
    for (int attempt = 0; attempt < 128; ++attempt) {
      const int m = std::uniform_int_distribution<int>(0, members_ - 1)(*rng);
      double weight;
      const uint64_t range = MemberRange(m, t, &weight);
      if (weight > 0.0 && u01(*rng) < weight) {
        const uint64_t base = range * cfg_.range_size;
        const uint64_t off =
            std::uniform_int_distribution<uint64_t>(0, cfg_.range_size - 1)(*rng);
        return (base + off) % n_;
      }
    }
    return std::uniform_int_distribution<uint64_t>(0, n_ - 1)(*rng);
  }

  // Member |m| = slot i, chain position j. Returns its range and current weight.
  uint64_t MemberRange(int m, double t, double* weight) const {
    const int k = cfg_.chain > 1 ? cfg_.chain : 1;
    const int i = m / k;
    const int j = m % k;

    const uint64_t phase = Mix(cfg_.seed, static_cast<uint64_t>(i), 0) % 1000000ULL;
    const double shifted = t + static_cast<double>(phase) / 1e6 * cfg_.lifetime_s;
    const double gen_f = shifted / cfg_.lifetime_s;
    const uint64_t gen = static_cast<uint64_t>(gen_f < 0 ? 0 : gen_f);
    const double u = gen_f - static_cast<double>(gen);

    // Member j occupies the window [j*lag, j*lag + width] of the generation.
    const double lag = k > 1 ? cfg_.chain_lag : 0.0;
    const double width = 1.0 - lag * static_cast<double>(k - 1);
    const double start = lag * static_cast<double>(j);
    const double v = (u - start) / (width > 0.0 ? width : 1.0);
    *weight = Trapezoid(v, cfg_.ramp_frac);

    uint64_t r = Mix(cfg_.seed, static_cast<uint64_t>(i), gen) % n_ranges_;
    for (int step = 0; step < j; ++step) r = Successor(r);
    return r;
  }

  uint64_t n_ranges() const { return n_ranges_; }

 private:
  static double Trapezoid(double v, double f) {
    if (v <= 0.0 || v >= 1.0) return 0.0;
    if (f <= 0.0) return 1.0;
    if (v < f) return v / f;
    if (v > 1.0 - f) return (1.0 - v) / f;
    return 1.0;
  }

  uint64_t Successor(uint64_t r) const {
    return Mix(cfg_.seed ^ 0xA5A5A5A5A5A5A5A5ULL, r, 0) % n_ranges_;
  }

  static uint64_t Mix(uint64_t seed, uint64_t a, uint64_t b) {
    uint64_t h = seed ^ (a * 0x9E3779B97F4A7C15ULL) ^ (b * 0xC2B2AE3D27D4EB4FULL);
    h ^= h >> 33; h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33; h *= 0xc4ceb9fe1a85ec53ULL;
    h ^= h >> 33;
    return h;
  }

  uint64_t n_, n_ranges_;
  int members_ = 0;
  Lifecycle cfg_;
};

// Maps a request to a key index in [0, n).
class KeyChooser {
 public:
  KeyChooser(uint64_t n, KeyDist dist, double theta, double hotspot_frac,
             Dynamics dyn = Dynamics(), Lifecycle life = Lifecycle())
      : n_(n), dist_(dist), zipf_(n, theta > 0.0 ? theta : 1e-6),
        hotspot_(static_cast<uint64_t>(hotspot_frac * static_cast<double>(n))),
        dyn_(dyn), lifecycle_(n, life) {}

  // |elapsed_s| is seconds since the run started; it drives the dynamics.
  uint64_t Next(std::mt19937_64* rng, double elapsed_s = 0.0) const {
    switch (dist_) {
      case KeyDist::kUniform:
        return std::uniform_int_distribution<uint64_t>(0, n_ - 1)(*rng);
      case KeyDist::kScrambled: {
        const uint64_t r = zipf_.Next(rng);
        return FnvHash64(r) % n_;
      }
      case KeyDist::kLifecycle:
        return lifecycle_.Next(rng, elapsed_s);
      case KeyDist::kZipfContiguous:
      default:
        return (Origin(elapsed_s) + zipf_.Next(rng)) % n_;
    }
  }

  // Where the hot region starts at |elapsed_s|. Exposed so the offline tools
  // can label which ranges were hot without replaying the trace.
  uint64_t Origin(double elapsed_s) const {
    uint64_t origin = hotspot_;
    if (dyn_.shift_per_s != 0.0) {
      origin += static_cast<uint64_t>(dyn_.shift_per_s * elapsed_s) % n_;
    }
    if (dyn_.phases > 1 && dyn_.phase_period_s > 0.0) {
      const uint64_t phase =
          static_cast<uint64_t>(elapsed_s / dyn_.phase_period_s) % dyn_.phases;
      origin += phase * (n_ / dyn_.phases);
    }
    return origin % n_;
  }

 private:
  static uint64_t FnvHash64(uint64_t v) {
    uint64_t h = 14695981039346656037ULL;
    for (int i = 0; i < 8; ++i) {
      h ^= (v >> (i * 8)) & 0xff;
      h *= 1099511628211ULL;
    }
    return h;
  }

  uint64_t n_;
  KeyDist dist_;
  ZipfianRank zipf_;
  uint64_t hotspot_;
  Dynamics dyn_;
  LifecycleChooser lifecycle_;
};

// 16-byte zero-padded decimal: lexicographic order == numeric order, so the
// order-preserving key -> range-id mapping Leaper needs is just integer
// division. Keep this format stable across all Leaper experiments.
inline void EncodeKey(uint64_t index, char* buf16) {
  std::snprintf(buf16, 17, "%016llu", static_cast<unsigned long long>(index));
}

inline std::string EncodeKey(uint64_t index) {
  char buf[17];
  EncodeKey(index, buf);
  return std::string(buf, 16);
}

}  // namespace leaper_bench

#endif  // LEAPER_BENCH_KEYGEN_H_
