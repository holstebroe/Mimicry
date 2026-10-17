# 04 — Optimizer

Three modes share one engine: **guided** (names and units set the plan), **unsupervised** (plain
global search), and **hybrid** (default: guided plan, but measured sensitivity overrides names).

## 1. Encoding

Every free parameter is `u ∈ [0,1]`, decoded per parameter [S: Acidus unit box with log scaling]:

- linear or **log** (log when the value text has Hz/ms/s units, or max/min >= 20 with min > 0, or the
  probe says the effect is uniform in log) [D]. This decode applies to CLAP plain values; VST3
  parameters use the plugin's native normalised coordinate (02 §1);
- stepped parameters are carried continuous in the strategy and rounded at decode;
- enums and booleans are *categorical*: handled by outer enumeration (§5), not by CMA-ES;
- decode clips to [0,1] and the loss adds a quadratic penalty for the clip distance, so the strategy
  does not drift outside the box [D].

## 2. Semantics

`ParamSemantics` classifies each parameter from name + module path + units text into
`osc, filter, env, lfo, fx, amp, global, unknown`, with a confidence. Token examples [D]:
osc: osc, wave, pitch, tune, detune, unison, sub, noise; filter: cutoff, freq, res, reso, q, drive,
lpf/hpf; env: attack, decay, sustain, release, adsr, env amt; lfo: lfo, rate, depth, mod; fx: reverb,
delay, chorus, mix, wet, feedback, size; amp: volume, gain, level, pan.

Names are **priors only**. The probe (§3) measures what each parameter does, and a measured signature
overrides a name guess. Names help most for ordering stages and for the first sigma.

## 3. Sensitivity probe (`mimicry probe`)

1. Render the plugin at defaults, twice (repeatability floor, 03 §6).
2. For each parameter, render at 5 values (0, 25, 50, 75, 100 %), others at default. Record the
   feature deltas: centroid (brightness), f0 shift, decay time, noisiness, level, tail length.
3. Classify: *dead* (no change above the floor; frozen at default and removed from the search),
   *pitch*, *brightness*, *time*, *level*, *spectrum*, *fx*; and log vs linear from the shape of the
   response.
4. Crashes and invalid outputs here populate the **safe box** (05 §3).
5. Pairwise interaction probes for the parameters of the strongest groups, as Acidus's 2×2 probes
   do [S]: `I = (y[A1,B1] − y[A0,B1]) − (y[A1,B0] − y[A0,B0])`. Pairs with I within the floor are
   treated as separable (fitted independently in stages); others stay in one stage.

Cost: about `5·n + pair probes` renders, a few minutes for a 100-parameter synth. Cached in the profile.

## 4. Pipeline

| Stage | Frees | Purpose |
| --- | --- | --- |
| S0 | none | Target analysis (f0, onset, kind, floor); set note plan; measure/apply pitch directly |
| S1 | all active | Seed: Sobol/LHS sample N (default 400) inside the safe box, plus fingerprint nearest neighbours if a database exists, plus defaults; keep the best K (default 8) |
| S2 | osc + amp (+ level) | Spectrum and level skeleton |
| S3 | + filter + env | Brightness and shape over time |
| S4 | + lfo + fx | Modulation and tails (long renders last, since they cost most) |
| S5 | all active | Joint polish: CMA-ES (n <= 64) or sep-CMA-ES (n > 64), small sigma |
| S6 | categorical outer loop | Enumerate the few enum parameters that matter most (<= 3, <= 4 values each), inner S5 |
| S7 | none | Snap stepped values, coordinate refinement, holdout check |

A stage frees only its group plus what it shares with earlier stages; the rest keep the value from the
previous stage [S: Acidus `STAGE_ONLY`]. Unsupervised mode skips S2-S4 and runs S1, S5, S6, S7 over
everything. `--stages` and `--only` / `--fix` select stages and parameters as in Acidus [S].

## 5. CMA-ES

Implementation follows Acidus's `CMAES` class [S], ported to C++ with a small Jacobi symmetric
eigensolver (no linear-algebra library):

- population `lam = 4 + floor(3 ln n)`, `mu = lam/2`, log weights, standard `cc, cs, c1, cmu, damps`;
- restart from the best point with `sigma0 · 0.5` and population doubled (cap 64) when
  `sigma < 1e-3` or condition number > 1e12 [S];
- stop on time cap (default 20 min [S]), patience without improvement (default 4 min [S]), or an
  evaluation budget; progress line every 15 s [S];
- first sigma by stage: 0.3 for S2-S4, 0.1 for S5 [D];
- **sep-CMA-ES** (diagonal covariance) above 64 parameters, since full covariance costs O(n²)
  memory and O(n³) eigen work [D];
- independent restarts **R** (default 3) from different seeds; the final answers are clustered in
  parameter space and in feature space to give a **top-K diverse** set (default K = 3) [A].

Batch evaluation: each generation's candidates are dispatched to the worker pool in parallel; the
generation waits for all or for a per-generation timeout, and any missing candidate is an invalid
outcome (05 §2).

## 6. Regularisation

A tie-breaker only: `λ · mean((u − u_default)²)` over active parameters with `λ` set to 0.5 % of the
current loss scale [D]. It prefers the simpler of two equally good presets and keeps results musical.
`--no-prior` turns it off.

## 7. Surrogate and fingerprint [A]

- **Fingerprint database**: `mimicry fingerprint` renders thousands of random valid presets once per
  plugin and stores parameter vector + a compact feature embedding (mean/std log-mel, centroid,
  flatness, envelope summary, ~128 floats). A new target starts from its nearest neighbours; this
  turns S1 from random into informed and is reusable across targets.
- **Surrogate (later)**: for plugins that render slowly (> 1 s per candidate), fit a Gaussian process
  on the active subset and let it propose candidates (Bayesian optimisation) between CMA-ES
  generations.

## 8. Plugin-to-plugin mapping [A]

After >= 50 solved (source params → target params) pairs, fit per target parameter a small monotone
model from the most correlated source parameter(s). Used as the warm start for the next presets and
exportable as `knob_map.json` (the cutoff law between two synths, in effect).
