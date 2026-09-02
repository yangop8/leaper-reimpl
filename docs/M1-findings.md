# M1 — Offline pipeline: key range selection, features, model

> **Scope note.** These measurements come from a clean-room reimplementation on
> LevelDB/RocksDB with synthetic workloads and NVMe (slow storage is emulated).
> The paper's results come from X-Engine, real Tmall and DingTalk traffic, and
> spinning disks. The numbers below describe *this* setup and are not a
> replication of the paper's. See the top of the repository README.

This milestone reimplements Section 4 of the paper (Algorithm 1, Algorithm 2,
the 18-dimensional feature vector, the LightGBM classifier) and evaluates it
against the paper's own offline baseline — "read in the last interval implies
read in the next" — which scored 0.83 recall there.

Everything below is measured on traces captured by `leaper_bench --trace_out`.

## The workload had to be built before the model could be evaluated

A stationary zipfian stream cannot evaluate this model. Measured on a 500k-key
zipf-0.99 trace, the naive baseline scores **precision = recall = AUC =
1.0000**: every range hot in one interval is hot in the next, so a learned model
can at best tie it. Adding a drifting hotspot does not fix it either — drift of
4000 keys/s against a 10^4 key range size moves about one range per interval.

The paper's baseline scores 0.83 recall, so roughly 17% of the ranges hot in one
interval were not hot in the previous one. Reproducing that churn is a
prerequisite, not a detail, and pure random churn will not do: a memoryless hot
set is unpredictable for any model, learned or not. The churn has to have
*shape*. Two shapes are generated (`--key_dist=lifecycle`), one per feature
family:

* **lifecycle** — a range ramps up, plateaus and decays over `--life_lifetime_s`.
  Six intervals of arrival-rate history say where a range sits on that arc; the
  last interval alone does not.
* **chains** — within one lifetime a slot activates `--life_chain` ranges
  `A, f(A), f(f(A)) ...`, each lagged by `--life_chain_lag` of the lifetime, so
  they are hot *together* with one leading the others.

The chain design went through one wrong version worth recording. The first
attempt made chain members *successive occupants* of a slot: A dies, f(A) is
born. That is unusable, because Algorithm 2 keeps a candidate only if the cosine
similarity of the two arrival-rate vectors exceeds a threshold, and ranges hot
in disjoint windows have near-orthogonal vectors. The paper's precursor is an
*overlapping lead* — "the probability of buying a piano rack rises after buying
a piano", where both are active in the same window — not a hand-off.

## Three leaks found in our own pipeline

Each of these inflated the model's apparent advantage and had to be fixed before
any number was worth reporting.

1. **Early stopping on the test split.** The boosting round was selected on the
   same rows the metrics were computed from. Fixed with a three-way
   chronological split: train / valid (early stopping) / test.
2. **Precursor selection over the whole trace.** Algorithm 2 chose each range's
   precursors using arrival-rate vectors that included the test period, so the
   selected precursor co-varies with the target *in the test window* by
   construction. On a control workload with **no planted precursor relation at
   all**, the leaky version still credited the precursor features with +1.2 AUC.
   After restricting Algorithm 2 to the training window that gain disappears
   (and goes slightly negative), which is the correct answer for a control.
3. **Key range selection over the whole trace.** Same issue, milder; Algorithm 1
   is now fitted on training slots only.

The split must be chronological, never random: adjacent intervals of the same
range are near-duplicates, so a random split leaks the label directly.

## Finding — Algorithm 1 does not terminate on *these synthetic* workloads

> **Correction (M8).** On real traces it does. On three Twitter cache-trace
> samples the paper's efficiency criterion stops on its own (A* = 300, 190,
> 60), so the non-termination below is a property of the synthetic access
> matrix — which reaches a stable occupancy — and not a gap in the algorithm.
> The sentence in this section calling it "a gap in the published algorithm"
> is withdrawn; see `docs/M8-review-followup.md`.

The paper's stopping rule is: keep doubling the key range size A while
`2*Z(2A) > alpha*Z(A)`, with alpha = 0.6. (Algorithm 1 line 2 defines Z as a
zero *count* while Definition 1 phrases it as a *proportion*; the two give the
same test, since doubling A halves the column count.)

On a zipf-0.99 trace the zero proportion falls from 0.73 to 0.20 as A goes
10 → 320 and then stops moving:

| A | ranges | Z_prop | ratio |
|---|---|---|---|
| 160 | 3,125 | 0.2265 | 0.741 |
| 320 | 1,563 | 0.2047 | 0.904 |
| 640 | 782 | 0.2019 | 0.986 |
| 1280 | 391 | 0.1998 | 0.990 |
| 2560 | 196 | 0.1991 | 0.996 |

Past A = 320 coarsening merges occupied cells with occupied and empty with
empty, so no zeros are lost, the ratio sits at 0.9-1.0, and the test keeps
saying "expand" — all the way to **A\* = 249,970, three key ranges for the whole
database**. The zero proportion simply stops carrying information about
granularity once the grid is coarser than the structure of the access pattern.

We add a floor on the range count (`--min_ranges`, default 1024, chosen to land
where the paper reports: a 10^4 key range size over a 10-20M row table is
roughly 1000-2000 ranges). **The reported A\* is then determined entirely by that
floor, not by the paper's criterion** — on the lifecycle workload the recovered
A\* = 4140 is close to the generator's true 4000, but `key_space / min_ranges` is
4149, so that agreement is arithmetic, not evidence. This should be treated as a
gap in the published algorithm rather than a reproduction detail.

## Ground truth for Algorithm 2

`tools/check_precursors.py` replays the generator's hash in Python to recover
the planted `A -> f(A)` pairs, then asks how often the true precursor is among
the gamma that Algorithm 2 selected. Without this, "the precursor features
contributed X" is unfalsifiable.

First measurement, on a trace with 1000 generator ranges where chain heads were
drawn afresh each generation:

```
[truth] 610 ranges active in the training window
[truth] 488 targets have both a selected and a true precursor
[truth] Algorithm 2 recall@gamma=3: 15/488 = 0.031   (chance = 0.008)
[truth] top-3 by transfer count alone: 12/488 = 0.025
```

Four times better than chance and useless in absolute terms. The cause is not
the cosine filter — the raw transfer counts do no better — but the sampling: 64
slots over roughly 10 generations of training gives about 640 chain draws spread
over 1000 ranges, so each specific `(A, f(A))` pair is observed **less than once**.
The paper's precursors are stable relations observed many times.

Rerun with 100 generator ranges and an 8 s lifetime, so each relation recurs
about five times in the training window:

| | control (`chain=1`) | treatment (`chain=4`) |
|---|---|---|
| planted relation | none | `A -> f(A)`, lagged by 0.2 of a lifetime |
| Algorithm 2 recall@3 | **0.000** | **0.708** |
| chance level | 0.058 | 0.064 |
| top-3 by transfer count alone | 0.000 | 0.250 |

Two things follow. **Algorithm 2 works** — it recovers 71% of a planted relation
against a 6% chance level, and correctly finds nothing in a control where
nothing was planted. And **the cosine-similarity filter is what makes it work**:
ranking by transfer count alone recovers only 25%, so the filter more than
doubles recovery. That is a direct validation of a design choice the paper
states but does not isolate.

## Results

Both runs: 4M keys, 100 generator ranges of 40k keys, 16 chains, 8 s lifetimes,
480 s at 40k ops/s, 1 s analysis intervals, range size pinned to the generator's
40k. The control and treatment have different positive rates (4.2% vs 22.9%), so
only within-run comparisons are meaningful.

Metrics are taken at an untuned 0.5 cutoff plus two threshold-free summaries.
`P@baseR` is where the model's precision-recall curve reaches the baseline's
recall — a read-off from the curve, labelled as such, not a tuned operating
point. Tuned cutoffs were tried and dropped: maximising validation F1 collapses
to "predict everything positive" when the model is miscalibrated on that window,
and matching the baseline's validation recall drags the cutoff to zero for the
same reason. Neither is a property of the model.

### Control — `chain=1`, no precursor structure

| | precision | recall | AUC | AP | P@baseR |
|---|---|---|---|---|---|
| baseline (last-slot rule) | 0.8988 | 0.8915 | 0.9449 | 0.8758 | — |
| LightGBM R | 0.9673 | 0.8896 | 0.9434 | 0.8915 | 0.9197 |
| LightGBM R+W | 0.9593 | 0.8901 | 0.9451 | 0.8954 | 0.9080 |
| LightGBM R+W+T | 0.9964 | 0.6603 | 0.9481 | 0.8990 | 0.9335 |
| LightGBM R+W+T+P | 0.9620 | 0.8457 | 0.9497 | 0.8991 | **0.8794** |

Adding precursor features moves AP by +0.0001 and *lowers* P@baseR by 0.054.
That is the right answer: there is no relation to find, so the features are
noise. Before the leak in Algorithm 2 was fixed, this same control credited them
with +1.2 AUC.

### Treatment — `chain=4`, planted precursor structure

| | precision | recall | AUC | AP | P@baseR |
|---|---|---|---|---|---|
| baseline (last-slot rule) | 0.8343 | 0.8346 | 0.9076 | 0.8504 | — |
| LightGBM R | 0.9929 | 0.7973 | 0.9230 | 0.8923 | 0.7764 |
| LightGBM R+W | 0.9946 | 0.7976 | 0.9242 | 0.8933 | 0.7898 |
| LightGBM R+W+T | 0.9925 | 0.7979 | 0.9257 | 0.9040 | 0.8072 |
| LightGBM R+W+T+P | 0.9857 | 0.8073 | 0.9360 | **0.9175** | **0.9254** |

The baseline's 0.8346 recall lands almost exactly on the 0.83 the paper reports
for the same rule, which is some evidence that the lifecycle workload sits in
the regime the paper's real workloads occupy.

Here the precursor features add +0.0135 AP and **+0.118 precision at the
baseline's recall**, against −0.040 in the control. Each feature family
contributes in the same order the paper's ablation reports.

### Where the model's advantage comes from

The naive rule is wrong exactly on transitions, so the transition mix bounds
what any model can win. Model accuracy per transition type (treatment):

| transition | share of test rows | baseline | model |
|---|---|---|---|
| steady hot | 19.1% | 100% | 95.8% |
| steady cold | 73.3% | 100% | 99.8% |
| **death** (hot, then cold) | 3.8% | 0% | **96.4%** |
| **birth** (cold, then hot) | 3.8% | 0% | 4.8% |

The model recovers almost every death and almost no birth, and that is
mechanistically what it should do: a range's decaying arrival rate over six
intervals says it is on its way out, while a birth in this generator is a fresh
draw with nothing in the history to foreshadow it. So the gain shows up as
**precision, not recall** — which is the half of the trade-off that decides how
much cache a prefetch wastes, and exactly why the paper wants both.

That also sets an expectation for M2-M4: on a workload whose births are
genuinely unpredictable, no prefetcher can raise the hit ratio much above what
the last-interval rule already gets. The paper's recall gain must come from real
workloads where births *are* foreshadowed — by precursors, by periodicity, or by
ramp-up that starts below the detection threshold. Our precursor treatment is
the only one of the three we can currently generate.
