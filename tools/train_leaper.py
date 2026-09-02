#!/usr/bin/env python3
"""Leaper offline pipeline (M1): key range selection, features, model training.

Implements Section 4 of the paper against a trace captured by leaper_bench:

  Algorithm 1  key range selection by efficient expansion
  Algorithm 2  precursor selection by transfer count + cosine similarity
  Section 4.2  the 18-dimensional feature vector
  Section 4.3  LightGBM with the paper's hyperparameters

and evaluates against the paper's own offline baseline -- "accessed in the last
interval => accessed in the next interval" -- which scored 0.83 recall there.
That number is the bar: whatever the learned model adds has to be measured
against it, not against zero.

Usage:
  python3 tools/train_leaper.py --trace=experiments/results/m1 [options]
"""

import argparse
import glob
import json
import os
import sys

import numpy as np

# --------------------------------------------------------------------------
# Trace loading


def load_meta(prefix):
    meta = {}
    path = prefix + ".meta"
    if os.path.exists(path):
        with open(path) as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                k, _, v = line.partition("=")
                meta[k] = v
    return meta


def load_trace(prefix):
    """Returns (t_ms, key, op) arrays plus a list of per-thread slices.

    Per-thread boundaries are kept because the precursor transfer matrix must
    be built from within-thread access order; merging N concurrent threads into
    one sequence manufactures adjacency that no single client ever produced.
    """
    files = sorted(glob.glob(prefix + ".trace.*"))
    if not files:
        sys.exit(f"no trace files matching {prefix}.trace.*")
    per_thread = []
    for f in files:
        raw = np.fromfile(f, dtype=np.uint32)
        if raw.size % 2:
            raw = raw[:-1]
        raw = raw.reshape(-1, 2)
        t_ms = raw[:, 0].astype(np.int64)
        key = (raw[:, 1] >> 2).astype(np.int64)
        op = (raw[:, 1] & 3).astype(np.int8)
        order = np.argsort(t_ms, kind="stable")
        per_thread.append((t_ms[order], key[order], op[order]))
        print(f"  {os.path.basename(f)}: {len(t_ms):,} records")
    t_ms = np.concatenate([p[0] for p in per_thread])
    key = np.concatenate([p[1] for p in per_thread])
    op = np.concatenate([p[2] for p in per_thread])
    return t_ms, key, op, per_thread


# --------------------------------------------------------------------------
# Algorithm 1: key range selection


def zero_count(pairs, n_slots, n_ranges):
    """Number of zero cells in the slot x range access matrix."""
    return n_slots * n_ranges - len(pairs)


def select_key_range(slot, key, n_slots, key_space, init_a, alpha,
                     min_ranges=1024, verbose=True):
    """Algorithm 1, on read accesses only.

    The paper defines Z as "the number of zeroes in M" (Algorithm 1 line 2) but
    Definition 1 phrases the efficiency test in terms of the *proportion* of
    zeros. The two are the same test: doubling the range size halves the column
    count, so 2*Z_count(2A) > alpha*Z_count(A) is exactly
    Z_prop(2A)/Z_prop(A) > alpha. Counts are what is implemented here.

    DEVIATION FROM THE PAPER. The published stopping rule does not terminate on
    a workload whose access matrix reaches a stable occupancy. Measured on a
    zipf-0.99 stream over a 500k key space, the zero proportion falls from 0.73
    to 0.20 as A goes 10 -> 320 and then plateaus:

        A       ranges   Z_prop   ratio
        320       1563   0.2047   0.904
        640        782   0.2019   0.986
        1280       391   0.1998   0.990
        2560       196   0.1991   0.996

    Past that point coarsening merges occupied cells with occupied and empty
    with empty, so no zeros are lost, the ratio sits at 0.9-1.0, and the test
    (ratio > 0.6) keeps saying "expand" all the way to A* = 249,970 -- three key
    ranges for the whole database, which is useless for prediction. The zero
    proportion simply stops carrying information about granularity once the
    grid is coarser than the structure of the access pattern.

    We therefore add a floor on the number of ranges. The default of 1024 puts
    the result in the same range the paper reports (a 10^4 key range size over
    a 10-20M row table is roughly 1000-2000 ranges). The unconstrained stopping
    point is still reported so the deviation is visible.

    Candidates are restricted to multiples of init_a so that the coarser
    matrices can be derived exactly from the finest one instead of rescanning
    the trace: for A = m * init_a, key // A == (key // init_a) * init_a // A.
    """
    base = np.unique(slot * (key_space // init_a + 1) + key // init_a)
    base_slot = base // (key_space // init_a + 1)
    base_rng = base % (key_space // init_a + 1)

    def zeros_for(a):
        assert a % init_a == 0
        coarse = (base_rng * init_a) // a
        n_ranges = int(key_space // a) + 1
        pairs = np.unique(base_slot * n_ranges + coarse)
        return zero_count(pairs, n_slots, n_ranges), n_ranges

    a = init_a
    z_a, n_a = zeros_for(a)
    bounded = False
    while True:
        two_a = a * 2
        if two_a > key_space:
            break
        z_2a, n_2a = zeros_for(two_a)
        efficient = 2 * z_2a > alpha * z_a
        if verbose:
            zp = z_a / (n_slots * n_a)
            zp2 = z_2a / (n_slots * n_2a)
            print(f"    A={a:<9} ranges={n_a:<8,} Z_prop={zp:.4f} "
                  f"ratio={zp2 / zp if zp else float('nan'):.3f} -> "
                  f"{'expand' if efficient else 'stop'}")
        if not efficient:
            break
        if n_2a < min_ranges:
            bounded = True
            if verbose:
                print(f"    stopping at A={a}: expanding would leave {n_2a:,} "
                      f"ranges, below --min_ranges={min_ranges}. The paper's "
                      f"criterion alone would keep expanding.")
            break
        a, z_a, n_a = two_a, z_2a, n_2a
    if verbose and not bounded:
        print("    stopped on the paper's efficiency criterion")

    # Binary search for the largest A* in [a, 2a] (multiple of init_a) with
    # A* * Z(A*) > alpha * a * Z(a).
    target = alpha * a * z_a
    max_a = min(2 * a, key_space, max(init_a, key_space // max(min_ranges, 1)))
    lo, hi = a // init_a, max_a // init_a
    best = a
    while lo <= hi:
        mid = (lo + hi) // 2
        cand = mid * init_a
        if cand <= 0:
            break
        z_c, _ = zeros_for(cand)
        if cand * z_c > target:
            best = cand
            lo = mid + 1
        else:
            hi = mid - 1
    return best


# --------------------------------------------------------------------------
# Algorithm 2: precursors


def compute_precursors(per_thread, range_size, n_ranges, rates, gamma, eps,
                       train_until_ms=None):
    """Algorithm 2: rank candidates by transfer count, keep those whose arrival
    rate vector is cosine-similar above eps, up to gamma per range.

    Both inputs must be restricted to the training window. Selecting a range's
    precursor using arrival rates that include the test period is leakage: the
    chosen precursor is one that co-varies with the target *in the test window*,
    so its rate in slot s-1 predicts the target in slot s partly by
    construction. Measured on a control workload with no precursor structure at
    all, the leaky version still credited the precursor features with +1.2 AUC.
    """
    transfer = np.zeros((n_ranges, n_ranges), dtype=np.int64)
    for t_ms, key, op, in ((p[0], p[1], p[2]) for p in per_thread):
        if train_until_ms is not None:
            keep = t_ms <= train_until_ms
            t_ms, key, op = t_ms[keep], key[keep], op[keep]
        seq = (key[op == 0] // range_size).astype(np.int64)
        if seq.size < gamma + 1:
            continue
        for j in range(1, gamma + 1):
            np.add.at(transfer, (seq[j:], seq[:-j]), 1)
    np.fill_diagonal(transfer, 0)

    norms = np.linalg.norm(rates, axis=1)
    norms[norms == 0] = 1.0
    unit = rates / norms[:, None]

    precursors = np.full((n_ranges, gamma), -1, dtype=np.int64)
    for r in range(n_ranges):
        cand = np.argsort(-transfer[r])
        cand = cand[transfer[r][cand] > 0][: gamma * 8]
        if cand.size == 0:
            continue
        sims = unit[cand] @ unit[r]
        keep = cand[sims > eps][:gamma]
        precursors[r, : keep.size] = keep
    return precursors, transfer


# --------------------------------------------------------------------------
# Featurisation


def build_dataset(read_rate, write_rate, precursors, history, slot_s, gamma, step=1,
                  clock_offset_s=0.0):
    """18 features per (range, slot): 6 read rates, 6 write rates, 3 timestamp
    components, gamma precursor read rates in the previous slot. Label is
    "read at least once in the next slot"."""
    n_ranges, n_slots = read_rate.shape
    rows, labels, slots = [], [], []
    for s in range(history, n_slots - step):
        rh = read_rate[:, s - history:s]                    # (R, history)
        wh = write_rate[:, s - history:s]
        # Prediction time on the *plug-in's* clock. The trace starts at the
        # measurement window but the online clock starts at the run, so the
        # timestamp features were offset by the warmup (30 s) between training
        # and inference until the trace meta began recording the offset.
        t = (s + 1) * slot_s + clock_offset_s
        stamp = np.tile(
            np.array([(t // 3600) % 24, (t // 60) % 60, t % 60], dtype=np.float32),
            (n_ranges, 1))
        prec = np.zeros((n_ranges, gamma), dtype=np.float32)
        for g in range(gamma):
            idx = precursors[:, g]
            have = idx >= 0
            prec[have, g] = read_rate[idx[have], s - 1]
        rows.append(np.hstack([rh, wh, stamp, prec]).astype(np.float32))
        labels.append((read_rate[:, s + step - 1] > 0).astype(np.int8))
        slots.append(np.full(n_ranges, s, dtype=np.int32))
    X = np.vstack(rows)
    y = np.concatenate(labels)
    s = np.concatenate(slots)
    names = ([f"read_rate_t-{i}" for i in range(history, 0, -1)] +
             [f"write_rate_t-{i}" for i in range(history, 0, -1)] +
             ["hour", "minute", "second"] +
             [f"precursor_rate_{i}" for i in range(gamma)])
    return X, y, s, names


# --------------------------------------------------------------------------


def metrics(y_true, y_pred, y_score=None):
    tp = int(np.sum((y_true == 1) & (y_pred == 1)))
    fp = int(np.sum((y_true == 0) & (y_pred == 1)))
    fn = int(np.sum((y_true == 1) & (y_pred == 0)))
    precision = tp / (tp + fp) if tp + fp else 0.0
    recall = tp / (tp + fn) if tp + fn else 0.0
    out = {"precision": precision, "recall": recall,
           "f1": 2 * precision * recall / (precision + recall) if precision + recall else 0.0}
    if y_score is not None:
        try:
            from sklearn.metrics import roc_auc_score, average_precision_score
            out["auc"] = float(roc_auc_score(y_true, y_score))
            out["ap"] = float(average_precision_score(y_true, y_score))
        except Exception:
            pass
    return out


def precision_at_recall(y_true, score, target_recall):
    """Precision where the model's PR curve reaches |target_recall|."""
    from sklearn.metrics import precision_recall_curve
    precision, recall, _ = precision_recall_curve(y_true, score)
    ok = recall >= target_recall
    return float(precision[ok].max()) if ok.any() else float("nan")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--trace", required=True, help="trace prefix passed to --trace_out")
    ap.add_argument("--slot_s", type=float, default=10.0, help="statistical time interval t")
    ap.add_argument("--init_range", type=int, default=10, help="initial key range size A")
    ap.add_argument("--alpha", type=float, default=0.6, help="efficient-expansion threshold")
    ap.add_argument("--min_ranges", type=int, default=1024,
                    help="floor on the range count; see select_key_range docstring")
    ap.add_argument("--range_size", type=int, default=0, help="skip Algorithm 1 and use this")
    ap.add_argument("--gamma", type=int, default=3, help="number of precursors")
    ap.add_argument("--eps", type=float, default=0.5, help="cosine similarity threshold")
    ap.add_argument("--history", type=int, default=6, help="arrival rate feature length")
    ap.add_argument("--train_frac", type=float, default=0.75,
                    help="share of slots used for train+valid; the rest is test")
    ap.add_argument("--valid_frac", type=float, default=0.8,
                    help="share of the train+valid slots used for training")
    ap.add_argument("--ablation", action="store_true",
                    help="also train on R, R+W, R+W+T feature subsets (paper Fig. 8)")
    ap.add_argument("--steps", type=int, default=1,
                    help="train K multi-step models: model k predicts the k-th "
                         "interval ahead (paper Section 6.1)")
    ap.add_argument("--dump_eval", type=int, default=0,
                    help="write N test rows and LightGBM's scores so the C++ "
                         "scorer can be checked against this implementation")
    ap.add_argument("--out", default="")
    args = ap.parse_args()

    meta = load_meta(args.trace)
    print(f"[trace] {args.trace}  meta: {len(meta)} keys")
    t_ms, key, op, per_thread = load_trace(args.trace)
    print(f"[trace] {len(t_ms):,} records total, "
          f"{(t_ms.max() - t_ms.min()) / 1000.0:.0f}s span")

    slot = (t_ms // int(args.slot_s * 1000)).astype(np.int64)
    n_slots = int(slot.max()) + 1
    key_space = int(key.max()) + 1
    print(f"[trace] {n_slots} slots of {args.slot_s}s, key space {key_space:,}")

    # Fix the chronological split before anything is fitted, so that key range
    # selection and precursor selection can be restricted to training slots.
    lo, hi = args.history, n_slots - 2
    if hi <= lo:
        sys.exit("trace is too short for the requested --slot_s and --history")
    test_slot = lo + int((hi - lo) * args.train_frac)
    valid_slot = lo + int((hi - lo) * args.train_frac * args.valid_frac)
    train_until_ms = int((valid_slot + 1) * args.slot_s * 1000)
    print(f"[split] train slots <= {valid_slot}, valid <= {test_slot}, "
          f"test > {test_slot} (of {n_slots})")
    train_mask = slot <= valid_slot

    is_read = (op == 0) | (op == 3)   # point reads and scan seeks
    if args.range_size > 0:
        range_size = args.range_size
        print(f"[alg1] using --range_size={range_size}")
    else:
        print("[alg1] key range selection (reads only):")
        sel = is_read & train_mask
        range_size = select_key_range(slot[sel], key[sel], valid_slot + 1,
                                      key_space, args.init_range, args.alpha,
                                      args.min_ranges)
        print(f"[alg1] selected key range size A* = {range_size}")

    n_ranges = key_space // range_size + 1
    print(f"[alg1] {n_ranges:,} key ranges")

    rid = key // range_size
    read_rate = np.zeros((n_ranges, n_slots), dtype=np.float32)
    write_rate = np.zeros((n_ranges, n_slots), dtype=np.float32)
    np.add.at(read_rate, (rid[is_read], slot[is_read]), 1.0)
    np.add.at(write_rate, (rid[~is_read], slot[~is_read]), 1.0)

    print(f"[alg2] precursors (gamma={args.gamma}, eps={args.eps})")
    precursors, transfer = compute_precursors(
        per_thread, range_size, n_ranges, read_rate[:, :valid_slot + 1],
        args.gamma, args.eps, train_until_ms)
    n_with = int(np.sum(precursors[:, 0] >= 0))
    print(f"[alg2] {n_with:,}/{n_ranges:,} ranges got at least one precursor")

    clock_offset = float(meta.get("clock_offset_s", 0.0))
    if clock_offset:
        print(f"[data] timestamp features offset by {clock_offset:.0f}s to match the online clock")
    X, y, s, names = build_dataset(read_rate, write_rate, precursors,
                                   args.history, args.slot_s, args.gamma, 1, clock_offset)
    print(f"[data] {X.shape[0]:,} rows x {X.shape[1]} features, "
          f"positive rate {y.mean():.4f}")

    # Three-way chronological split (cutoffs fixed above). Early stopping needs
    # a validation set, and it must not be the test set: selecting the boosting
    # round on the test split is model selection on the test split.
    tr = s <= valid_slot
    va = (s > valid_slot) & (s <= test_slot)
    te = s > test_slot
    print(f"[data] chronological split: train<=slot {valid_slot} ({tr.sum():,}) | "
          f"valid<=slot {test_slot} ({va.sum():,}) | test ({te.sum():,})")
    if te.sum() == 0 or va.sum() == 0:
        sys.exit("valid or test split is empty; use a longer trace or smaller --slot_s")

    # Paper's baseline: read in the previous slot => read in the next slot.
    prev_hot = (X[:, args.history - 1] > 0).astype(np.int8)
    base = metrics(y[te], prev_hot[te], X[te, args.history - 1])
    print(f"[baseline] last-slot rule : precision={base['precision']:.4f} "
          f"recall={base['recall']:.4f} auc={base.get('auc', float('nan')):.4f} "
          f"ap={base.get('ap', float('nan')):.4f}")

    # Where the two can possibly differ. The naive rule is wrong exactly on
    # transitions, so the transition mix bounds what any model can win.
    tp_, ta_ = prev_hot[te], y[te]
    cats = {"steady_hot": (tp_ == 1) & (ta_ == 1), "death": (tp_ == 1) & (ta_ == 0),
            "birth": (tp_ == 0) & (ta_ == 1), "steady_cold": (tp_ == 0) & (ta_ == 0)}
    print("[transitions] test set composition:")
    for k, m in cats.items():
        print(f"    {k:<12} {int(m.sum()):>8,}  {100.0 * m.mean():5.2f}%")

    import lightgbm as lgb
    params = dict(objective="binary", num_leaves=31, learning_rate=0.05,
                  bagging_fraction=0.8, bagging_freq=1, feature_fraction=0.9,
                  verbose=-1, seed=42)

    groups = {
        "R": list(range(0, args.history)),
        "W": list(range(args.history, 2 * args.history)),
        "T": list(range(2 * args.history, 2 * args.history + 3)),
        "P": list(range(2 * args.history + 3, 2 * args.history + 3 + args.gamma)),
    }
    subsets = ([("R", ["R"]), ("R+W", ["R", "W"]), ("R+W+T", ["R", "W", "T"]),
                ("R+W+T+P", ["R", "W", "T", "P"])] if args.ablation
               else [("R+W+T+P", ["R", "W", "T", "P"])])

    results, best = {}, None
    for label, keys in subsets:
        idx = sorted(i for k in keys for i in groups[k])
        sub_names = [names[i] for i in idx]
        dtrain = lgb.Dataset(X[tr][:, idx], label=y[tr], feature_name=sub_names)
        dvalid = lgb.Dataset(X[va][:, idx], label=y[va], feature_name=sub_names,
                             reference=dtrain)
        booster = lgb.train(params, dtrain, num_boost_round=500, valid_sets=[dvalid],
                            callbacks=[lgb.early_stopping(30, verbose=False)])
        # Reporting. The primary numbers are taken at the untuned 0.5 cutoff,
        # so nothing is fitted to the test split, plus two threshold-free
        # summaries (AUC and average precision, the latter being the honest one
        # under this class imbalance). Tuning the cutoff was tried and dropped:
        # maximising validation F1 collapses to "predict everything positive"
        # when the model is miscalibrated on that window, and matching the
        # baseline's validation recall drags the cutoff to ~0 for the same
        # reason -- both make the model look worse than it is at 0.5 and
        # neither is a property of the model.
        score = booster.predict(X[te][:, idx])
        pred = (score > 0.5).astype(np.int8)
        m = metrics(y[te], pred, score)
        m["threshold"] = 0.5
        # Where the model's PR curve sits at the baseline's recall. This is a
        # read-off from the test curve, not a tuned operating point, and is
        # labelled as such.
        m["precision_at_baseline_recall"] = precision_at_recall(
            y[te], score, base["recall"])
        results[label] = m
        print(f"[lightgbm {label:<8}] iters={booster.best_iteration:<4} "
              f"precision={m['precision']:.4f} recall={m['recall']:.4f} "
              f"auc={m.get('auc', float('nan')):.4f} ap={m.get('ap', float('nan')):.4f} "
              f"P@baseR={m['precision_at_baseline_recall']:.4f}")
        if label == subsets[-1][0]:
            best = (booster, idx, sub_names, pred)

    booster, idx, sub_names, pred = best
    print("[transitions] accuracy on each transition type "
          "(baseline is 100% on steady, 0% on transitions by construction):")
    for k, m in cats.items():
        if m.sum() == 0:
            continue
        acc = float((pred[m] == ta_[m]).mean())
        print(f"    {k:<12} model={acc:6.2%}  n={int(m.sum()):,}")

    imp = booster.feature_importance(importance_type="gain")
    total = imp.sum() or 1.0
    print("[lightgbm] feature importance (gain):")
    for n, v in sorted(zip(sub_names, imp), key=lambda kv: -kv[1])[:10]:
        print(f"    {n:<20} {100.0 * v / total:5.2f}%")
    lgbm = results[subsets[-1][0]]

    out = args.out or (args.trace + ".model")
    booster.save_model(out + ".txt")

    # Precursor table for the online predictor: "target p0 p1 p2".
    with open(out + ".precursors.txt", "w") as f:
        f.write("# target precursor... (Algorithm 2, fitted on training slots only)\n")
        for r in range(n_ranges):
            ps = [int(x) for x in precursors[r] if x >= 0]
            if ps:
                f.write(f"{r} " + " ".join(str(x) for x in ps) + "\n")

    # Multi-step models. Model k predicts "read in the k-th interval ahead";
    # the two-phase prefetcher unions steps 1..k1 for T1 and k1+1..k1+k2 for T2.
    all_idx = sorted(i for k in ["R", "W", "T", "P"] for i in groups[k])
    for step in range(2, args.steps + 1):
        Xs, ys, ss, _ = build_dataset(read_rate, write_rate, precursors,
                                      args.history, args.slot_s, args.gamma, step,
                                      clock_offset)
        trs, vas = ss <= valid_slot, (ss > valid_slot) & (ss <= test_slot)
        if vas.sum() == 0:
            print(f"[multistep] step {step}: validation split empty, stopping")
            break
        d1 = lgb.Dataset(Xs[trs][:, all_idx], label=ys[trs], feature_name=names)
        d2 = lgb.Dataset(Xs[vas][:, all_idx], label=ys[vas], feature_name=names,
                         reference=d1)
        bs = lgb.train(params, d1, num_boost_round=500, valid_sets=[d2],
                       callbacks=[lgb.early_stopping(30, verbose=False)])
        bs.save_model(f"{out}.step{step}.txt")
        tes = ss > test_slot
        ms = metrics(ys[tes], (bs.predict(Xs[tes][:, all_idx]) > 0.5).astype(np.int8))
        print(f"[multistep] step {step}: precision={ms['precision']:.4f} "
              f"recall={ms['recall']:.4f} -> {out}.step{step}.txt")
    import shutil
    shutil.copyfile(out + ".txt", out + ".step1.txt")

    if args.dump_eval > 0:
        # Cross-implementation check data: the C++ scorer must reproduce these.
        rows = np.where(te)[0][: args.dump_eval]
        sc = booster.predict(X[rows][:, idx])
        with open(out + ".eval.csv", "w") as f:
            f.write(",".join(sub_names) + ",lgb_score\n")
            for i, r in enumerate(rows):
                f.write(",".join(f"{v:.6g}" for v in X[r][idx]) + f",{sc[i]:.10g}\n")
        print(f"[out] wrote {out}.eval.csv ({len(rows)} rows) for the C++ scorer check")
    with open(out + ".json", "w") as f:
        json.dump({"range_size": range_size, "n_ranges": int(n_ranges),
                   "slot_s": args.slot_s, "history": args.history,
                   "gamma": args.gamma, "feature_names": names,
                   "baseline": base, "lightgbm": lgbm, "ablation": results,
                   "transitions": {k: int(v.sum()) for k, v in cats.items()},
                   "positive_rate": float(y.mean()), "meta": meta}, f, indent=2)
    print(f"[out] wrote {out}.txt and {out}.json")


if __name__ == "__main__":
    main()
