# 06 — Targets

A target answers one question for the loss: what should the render sound like?

## 1. WAV target (`mimicry fit`)

`WavTarget` analyses the file and fills a `TargetSpec`:

| Field | Source |
| --- | --- |
| kind | note / hit / texture / loop, from f0 confidence, onset sharpness, duration (override `--kind`) |
| f0, key, cents | harmonic-comb search + weighted least squares on parabolic-interpolated peaks [S: Acidus `estimate_f0`]; override `--note C3` |
| onset, gate, tail | envelope analysis; override `--gate 800ms` |
| velocity | default 100; `--velocity`; accent-like behaviour is searched only if the plugin has a velocity-to-timbre route (probe) |
| floor | noise floor measured in the silence (03 §2) |
| channels | mono mix by default |

Sample rates are reconciled by resampling the target to the render rate. Multi-note files need a
`--midi` file (notes, velocities, gates), played exactly as in Acidus's stage MIDI files [S]; without
it a file is treated as one event.

## 2. Plugin-to-plugin (`mimicry transfer`)

`PluginTarget` = a **source** plugin + source preset(s), rendered through a **probe plan**, matched by a
**target** plugin. **Source and target may be different formats**: CLAP to VST3, VST3 to CLAP, or the
same format on both sides. The source is only ever *rendered*, so its format affects nothing else, and
the target's parameters are searched in that format's own coordinate (02 §1).

**Source presets** come from state files, CLAP preset-load/discovery, VST3 program lists and
`.vstpreset` files, or a folder of any of these; `--all` iterates every factory preset the source
exposes.

**Probe plan** [D]: a set of renders that makes parameters identifiable, where one note would not:

| Axis | Values | Why |
| --- | --- | --- |
| key | C2, C3, C4 | key tracking, octave scaling |
| velocity | 60, 100, 127 | velocity response, accent |
| gate | 150 ms, 1000 ms (+ tail) | envelope shape, release |

A full cross is 18 notes; the default plan is a thinned subset of 9 plus the three octave
anchors. The plan is an editable JSON (`plans/default.json`). Source renders are cached per preset, so
a source is rendered once. The loss is the weighted sum over plan items, with **holdout** items
(e.g. key C#3 at velocity 80) excluded from the fit and scored afterwards (07 §2).

The source is exact and noise-free, so knob settings, note times and pitches are exactly known [S:
Acidus training plan, same observation]. Effects, drift and random options in the source are turned off
or pinned, otherwise the source is its own noise.

**Bank loop**: each source preset is an independent job, spread across the pool; each job warm-starts
from the previous best if the two source presets are close in feature space, else from the
fingerprint's nearest neighbours. Output: `bank-report.csv` (preset, fit loss, holdout loss, evals,
time, flags), a folder of target states (and `.vstpreset` files for VST3 targets, 07 §3), and a list of
presets that could not be matched, defined as loss above 1.5x the repeatability floor after the full
pipeline (typically a source feature the target lacks: an effect, a mod destination, a wavetable).

**Learned mapping**: see 04 §8. Mappings are keyed by (source format, source uid, target format,
target uid).

## 3. Image target (`mimicry image`) [A]

An image has no sound, so there are two honest routes; both are offered and the report says which
was used.

**Route A: descriptors (semantic).** `ImageFeatures` (shared with the image-seeded synth idea in this
project, see its notes) are extracted:

| Feature | Definition |
| --- | --- |
| luminance mean/contrast | histogram mean and spread |
| edge softness | mean gradient magnitude / Laplacian variance |
| colourfulness | Hasler–Süsstrunk |
| warmth | red/blue balance of mean colour |
| texture | local variance, flatness of the 2-D FFT |
| row profile | mean intensity per row (low to high) |
| column profile | mean intensity per column (left to right) |

`image_map.json` maps them to **audio descriptor targets**: brightness (centroid), flatness (noisiness),
attack and decay times, spectral tilt, roughness. Defaults are guesses [D] and meant to be edited.
Example: soft edges → low cutoff, as in the image-seeded synth idea. The loss is the distance between
the render's descriptors (03 `shape`) and the target descriptors, so there is no target audio.

**Route B: spectrogram (literal).** The image is the target spectrogram: rows are log frequency,
columns are time, intensity is dB over a chosen range. The loss is `bands`/`msstft` error against the
resized image after level alignment. `--duration` and `--fmin/--fmax` set the mapping.

An image underdetermines a sound, so the **top-K diverse results** matter most here (04 §5), and an
image can also seed the RNG deterministically so the same image always gives the same exploration
[A].

## 4. Presets as targets from other sources [A]

A target preset need not come from a plugin: any renderable source (a hardware recording, a
sample-based instrument, a DAW bounce) is just a WAV and goes through §1. The Acidus fits to a real
TB-303 are this case [S].

## 5. Effects (audio in, audio out) [A]

Effect plugins need an input signal. Three cases:

| Case | Input | Target |
| --- | --- | --- |
| Reamp | the user's dry WAV | the user's processed WAV (best case: exact) |
| Known source | a built-in signal (pink noise burst, sine sweep, impulse, saw, drum loop) | processed version of the same signal |
| Unknown dry | none | statistics only (tail length, spectral tilt, modulation depth): weak, flagged |

Tail length (reverb, delay) comes from the plugin's tail report (CLAP `clap.tail`, VST3
`getTailSamples`) and from the target's decay, and long tails make renders slow, so effect stages are
last (04 §4).
