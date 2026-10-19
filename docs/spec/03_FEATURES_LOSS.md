# 03 — Features and loss

The loss is built from the same families Acidus uses to fit a hardware 303 [S:
`calibrate_reference.py`], generalised from pitched 303 notes to arbitrary sounds. All errors are in
dB, lower is better, and all are insensitive to waveform phase unless stated.

## 1. Pre-processing

Mix to mono (stereo features in §5), remove DC from the target using the pre-note silence
[S], resample both to a common rate, align onsets (global onset offset is a fitted constant, as in
Acidus [S]).

## 2. Feature set

| Feature | Definition | Use |
| --- | --- | --- |
| `msstft` | Log-magnitude STFT at 512 / 2048 / 8192 points, hop N/4, mean |log X − log Y| | all sounds [D] |
| `harm` | Level of every harmonic (energy within ±f0/4 of k·f0) in the whole-note DFT, weighted k^-0.5 [S] | pitched |
| `inter` | Energy between harmonics: catches spurious/aliasing content [S] | pitched |
| `harmt` | Each harmonic's level over time: 40 ms frames every 10 ms, same bands and weights [S] | pitched, evolving |
| `env` | Short-time RMS envelope: window 2 pitch periods (5 ms if unpitched), 1 ms hop [S] | all |
| `bands` | 1/3-octave band levels over time, 23 ms frames [S] | filter sweeps, noise |
| `wave` | Phase-aligned waveform shape, 20 ms frames, per-frame LS gain, reported as 10·sqrt(residual/ref) [S] | very short or single-cycle targets only |
| `shape` | Spectral centroid, flatness, rolloff, flux, MFCC(13) distance [D] | texture, image descriptors |

Noise-floor clamp [S]: `harm`, `inter`, `msstft` and `bands` are clamped at the target's own noise
floor (the inter-harmonic level beside each harmonic; the quietest frame of each band), so a model that
rolls off cleanly below a noisy recording's hiss is not scored as missing harmonics, and `inter` counts
only content the candidate adds on top of that noise.

## 3. Weights by sample kind

`mimicry fit` classifies the target (note / hit / texture / loop) from f0 confidence, onset sharpness
and duration, then picks a weight preset [D, tune on real data]:

| Kind | msstft | harm | inter | harmt | env | bands | shape |
| --- | --- | --- | --- | --- | --- | --- | --- |
| note | 1 | 1 | 0.5 | 0.5 | 1 | 0.5 | 0 |
| hit | 1 | 0 | 0 | 0 | 1.5 | 1 | 0.25 |
| texture | 1 | 0 | 0 | 0 | 0.5 | 1 | 1 |
| loop | 1 | 0 | 0 | 0 | 1 | 1 | 0.5 |

`--w-<feature> N` overrides, as Acidus's `--w-harmt`, `--w-sweep` do [S]. The summary score is a
guide, not the verdict: it can rank a candidate ahead while a narrow resonance line is smeared, so the
A/B audio is always written (07 §1) [S: same caveat in Acidus's training plan].

## 4. Level

If the plugin has a parameter classified as output level (`volume`, `gain`, `level`, `output`), it
joins the free set and level is matched by it. Otherwise one global gain is **solved in closed form**
for each candidate (the dB offset that minimises the L1 log error over cells above the floor [S: one
global gain solved directly in Acidus]) and reported as `gain_db`; a non-zero residual means the preset
cannot reach the target loudness and the user trims in the DAW.

## 5. Stereo [A]

Phase 1 scores the mono sum. Later: add mid and side features and an inter-channel level/time-difference
term, with `--channel left|right|mix` as in Acidus's `fit_stage.py` [S].

## 6. Noise and repeatability

Before any fit, render the same preset twice [S: Acidus probe P12]. The difference is the plugin's
**repeatability floor** (random LFO phase, analog drift, unison spread). It sets (a) the clamp for
spectrogram cells, (b) the success threshold (a fit within 1.5x the floor is as good as the plugin can
repeat), and (c) how many renders to average per candidate (1 if the floor is below 0.5 dB, else up to
4). Plugins with a seed or "random" parameter get it pinned.

## 7. Cost

Feature extraction must stay well below render time. Targets [D]: a 2 s note costs under 5 ms of
features per candidate with cached target spectra and preallocated FFT plans. The target's features are
computed once; `RenderCache` keys on the exact parameter bytes, note plan, rate and block size so
duplicate candidates render once [S: Acidus `_job_key`].

## 8. Implementation details (WP2)

Where the sections above leave a number open, `src/features` uses these [D] unless noted:

- **Frames.** Only full frames are analysed; the candidate is shifted by the fitted onset offset and
  padded or cut to the target's length first. `msstft` is the mean over the three sizes; `bands` frames
  are the power of two nearest 23 ms (hop half a frame), 1/3-octave bands without a bin are dropped.
- **Floors.** `msstft`: per-bin 10th percentile over frames. `bands`: quietest frame per band [S]. `harm`:
  mean power of the two gaps beside the harmonic. On top of that nothing more than 90 dB below the
  target's own peak is scored (`dynamic_range_db`), for every log feature.
- **Gain.** Median (the L1 optimum) of target minus candidate over `msstft` 2048-point cells that are
  at least 3 dB above the floor in both. Digital silence in the candidate stays silent after the gain.
- **`inter`** is one-sided: only energy the candidate adds above the target's own gap level counts, and
  it is a mean over the gaps, so a single spur in a 64-harmonic comb costs a fraction of its height.
- **`env`** window is 2 pitch periods only when the target is scored as pitched (kind note, or `--note`).
- **`shape`** is the mean over active frames (2048 points, hop 512, within 60 dB of the loudest) of
  log2 centroid, flatness in dB, log2 rolloff (85 %), flux of unit-norm magnitudes and MFCC 1-13 of a
  26-band mel filterbank in dB (c0 is left out, so the feature is level independent). The loss is the mean
  of 6 x |d centroid|, |d flatness|, 6 x |d rolloff|, 10 x |d flux| and the RMS MFCC difference.
- **Kind** from the 1 ms envelope: note if f0 confidence >= 0.5; loop if >= 3 onsets (9 dB within 20 ms)
  and >= 2 s; hit if shorter than 1.5 s with a 30-to-3 dB attack under 20 ms; else texture.
- **f0** uses the loudest 64k samples, the 12 strongest peaks as comb seeds (also at 1/2 ... 1/8), a
  1 dB per missing harmonic penalty, and `confidence = 2 * (comb energy share - 0.5)`.
- **Repeatability.** Renders per candidate: 1 below 0.5 dB, else `ceil(floor / 0.5) + 1` clamped to 2-4;
  success threshold 1.5 x floor.
- **Render cache.** Key = exact bytes of the parameter vector, note plan, rate, block size, settle,
  tail, peak limit and the silent flag. Failed renders are cached (rule 6) except timeout and host
  error, which may be transient.
- **Cost.** Measured 17 ms (unpitched) to 27 ms (pitched) per 2 s candidate at 48 kHz on the build
  machine, against the 5 ms goal above; the three STFT families, the 40 ms `harmt` frames and the
  whole-note DFT dominate. Optimising is deferred until the optimizer exists (OQ2).

## 9. Effect features [A]

An effect render is scored with the same features against the target's output for the same input
signal (06 §5): `msstft`, `bands`, `env` and `shape` apply unchanged. For effects the loss adds, per
probe signal where it is informative: band gains of the response (impulse, sweep), harmonic levels
relative to the fundamental (sine at two levels), output level against input level (stepped sines), tail
decay (energy decay curve), and modulation rate and depth from the band envelopes of a sustained tone.
All are phase-insensitive (an LFO's start phase and a delay's polarity do not change them), and each
gets a T4 invariance case (07 §4) when it is implemented (CLAUDE.md rule 10).
