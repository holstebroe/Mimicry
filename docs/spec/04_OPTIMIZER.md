# 04 — Optimizer

Three modes share one engine: **guided** (names and units set the plan), **unsupervised** (plain
global search), and **hybrid** (default: guided plan, but measured sensitivity overrides names).

## 1. Encoding

Every free parameter is `u ∈ [0,1]`, decoded per parameter [S: Acidus unit box with log scaling]:

- linear or **log** (log when the value text has Hz/ms/s units, or max/min >= 20 with min > 0, or the
  probe says the effect is uniform in log) [D]. This decode applies to CLAP plain values; VST3
  parameters use the plugin's native normalised coordinate (02 §1), re-mapped by the optimizer only where
  the probe measures a log response (§9);
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

**Roles** [A] sit below the groups and name what a parameter does, independent of the plugin:
`cutoff, resonance, filter_env_amount, filter_key_track, filter_type, attack, decay, sustain, release,
osc_wave, osc_level, detune, coarse_tune, fine_tune, unison, noise_level, lfo_rate, lfo_depth, drive,
mix, feedback, time, size, damping, threshold, ratio, makeup, eq_freq, eq_gain, eq_q, volume, pan`.
A role is guessed from name tokens, units and value text (a "Hz" range from about 20 Hz to 20 kHz on a
filter-like name is `cutoff`) and **confirmed by the probe**: a cutoff moves the centroid smoothly and
roughly log-linearly, a release changes only the decay after note-off, an attack only the onset slope,
a mix crossfades with the dry input. A measured signature that contradicts the name drops the role's
confidence. Roles drive the parameter map (§8), the doctor report (08) and the stage order.

Names are **priors only**. The probe (§3) measures what each parameter does, and a measured signature
overrides a name guess. Names help most for ordering stages and for the first sigma. Pitch-only
parameters are pinned when S0 applies the target's pitch (§9).

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

**Effects** [A]: the probe renders the effect probe signals (06 §5) instead of notes and records effect
features: band gains and tilt of the response, THD at two levels, output level at stepped input levels
(dynamics), tail length, modulation rate and depth, self-noise. A parameter that changes nothing for any
signal is dead.

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

**Map without fitting** (`mimicry map --source A --target B`): both plugins are probed (or their
profiles read), and each target parameter is matched to the source parameter(s) with the same role and
the closest probe signature (the feature-delta curves of §3, compared after normalising each feature).
Where both value texts carry a unit (a cutoff in Hz, an attack in ms), the law is exact: read the
source value as text and invert the target's value text to get the target coordinate. Otherwise the
law is the monotone curve that aligns the two signature curves. Links can be many-to-one (a source
macro driving three target parameters) or one-to-many and are reported as such, not forced into pairs.
Every entry carries a confidence (role confidence × signature similarity), and unmatched parameters on
either side are listed.

**Learned map**: after >= 50 solved (source params → target params) pairs, fit per target parameter a
small monotone model from the most correlated source parameter(s), starting from the map above. Both
are used as the warm start for the next presets and exported as `knob_map.json` (the cutoff law
between two synths, in effect), together with each side's mapping to roles.

## 9. Implementation details (WP3)

Where the sections above leave a number or a rule open, `src/core` (`Cma`, `Sampling`, `ParamSemantics`)
and `src/opt` use these [D] unless noted:

- **Roles.** A parameter is *categorical* when it has 2 values, or 3-16 values whose value texts contain no
  digit and no units; other stepped parameters are carried continuous and rounded to their grid at decode.
  Fixed (never searched): read-only, bypass, program change, hidden, not automatable, single-valued,
  seed/random by name (spec 03 §6), dead by the probe, `--fix`, everything outside `--only`.
- **Pitch.** S0 applies the target's measured key + cents to the notes, so a fit of a pitched target pins
  parameters that only shift pitch: the probe's `pitch` class, or without a profile a name with tune,
  tuning, fine or pitch (not detune, often a unison spread). A pitch basin is a few cents wide and flat
  around it, which no spectral loss can search.
- **Warp.** A parameter whose probe response is `log` (more than half of the change in the first quarter of
  u) is searched in `x` with `u = (100^x − 1) / 99`, unless the backend already decodes it in log. This
  applies to **both** formats: the clap-wrapper build of the test synth exposes its 20 Hz-20 kHz cutoff
  linearly in the VST3 normalised value, which the probe flags (plan OQ9). The probe misses a time
  parameter it classes by level (a long attack lowers the level, linearly), so the plugin's own value texts
  decide too [A]: a continuous parameter whose texts at 0/25/50/75/100 % read as Hz/kHz or ms/s, lie on a
  straight line (each quarter point within 5 % of the span) and span at least a factor 100, is searched with
  the exact log map of that ratio, `u = (r^x − 1) / (r − 1)` with `r = max / min` (capped at 10^5), probe or
  not. Measured: the VST3 test fit put Attack (1 ms - 2 s, linear) at 1064 ms instead of 5 ms on 2 of 6
  seeds (2.06 dB); with the text warp all 6 seeds end below 0.6 dB.
- **Boundary penalty.** 100 dB per unit² of clip distance in `x`, added to the CMA-ES fitness only (the
  rendered point is the clipped one).
- **Invalid candidates** rank after all valid ones as `10^6 + 10^4 · severity + 100 · distance` to the run's
  best valid point (05 §3); more than half invalid shrinks sigma by 0.7 and recentres on the best valid point.
- **Budget.** S1 renders `min(N, budget / 4)` LHS points in the safe box (categoricals uniform), after the
  defaults and any given seeds (each conformed to the space first: fixed parameters at their defaults,
  the others clipped to the safe box and snapped to their grid [A]); S2-S4 get 12 % each of what is left (time and evaluations), S5 75 % of what
  is left, S6 inner runs 35 % each, S7 the rest. S5 run 1 polishes the best point with half of S5; runs
  2..R start from the next S1 seeds not yet used as a start or near a run's best (sigma 0.3) and split the
  other half. Patience applies per
  CMA-ES run, the time cap and evaluation budget globally.
- **Categorical sweep.** S1 and each group stage end with a one-at-a-time sweep of every categorical
  parameter on the best point (two passes at most), so S5 does not polish the wrong waveform. S6 then ranks
  categoricals with at most 4 values by the loss spread they cause, enumerates the top 3, and runs an inner
  S5 for the two best other combinations within 3 dB of the best.
- **S7** tries ±0.04 and then ±0.01 (one grid step for stepped parameters) on every active parameter and
  keeps the combination of the improving moves if it beats the best single move. The holdout check is WP6.2.
- **Interaction.** A pair the probe finds non-separable (interaction RMS above `2 · floor + 0.5`) with its
  parameters in different stages moves the later one into the earlier one's group.
- **Top-K.** The best of each CMA-ES run, S6 and S7 (up to 24) are re-scored with embeddings (one render
  each, cached; never the repeats of a noisy plugin; a failure is counted in the outcomes [A]); a candidate joins the top K unless it is within 0.1 RMS in `u` of one already chosen or within
  0.5 embedding units of it (it sounds the same).
- **Embedding** (77 floats, dB-like): 32 log-mel band means relative to their mean and their standard
  deviations over frames within 60 dB of the loudest, centroid (6 · log2 Hz) and flatness mean and standard
  deviation, the envelope at 10-1600 ms after the onset relative to the peak, and the level (left out of
  distances).
- **Probe.** Descriptors per render: centroid, f0 (confidence >= 0.5), peak to −20 dB decay, onset to
  −60 dB tail, flatness, level, embedding. Unit scores: 20 cents, 0.15 octave, 0.25 in log2 time, 1.5 dB;
  f0 changes by a small-integer ratio are estimator jumps, not pitch. Dead: embedding change at most
  `max(3 · floor, 0.15)`, level within 0.5 dB, pitch within 5 cents at every value that rendered; values
  that failed do not keep a parameter alive (a hazard switch that only fails at its top is frozen at its
  default, which rendered, and its failures stay in the profile) [A: measured, a zero-effect hazard left
  in the search cost the WP4 test fit 2-3 dB]. Otherwise the
  largest score >= 1 wins (`fx` when the tail grows twice as much as the decay and flatness moves 1.5 dB),
  else `spectrum`. The safe box is the run of good probe values on each side of the default; a
  failure next to the default narrows that side to the default itself [A]. Pairs: the 10
  strongest parameters, corners at 0.25 / 0.75.
- **Surrogate.** On when a candidate costs more than 1 s (or forced): a GP (RBF, length scale = median
  pairwise distance, standardised targets) on the last 150 valid points of the free subset that agree with
  the run on every searched parameter it holds [A] ranks 3λ
  samples by `mean − sd`, and the best λ are rendered.
- **Fingerprint file:** `profiles/<format>-<uid>-<version hash>.fingerprint`, magic `MIMFPDB1`, a JSON header
  line (ids, names, counts), then float32 rows of coordinates and embedding. A database is used only for
  the exact parameter table (ids and names) it was built for; S1 adds the 8 nearest neighbours.
