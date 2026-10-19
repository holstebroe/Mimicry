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

**Source presets** come from the harvest library (§6): state files, CLAP preset-load/discovery, VST3
program lists and `.vstpreset` files, a folder of any of these, or presets captured by hand; `--all`
iterates every preset the harvest found. Each is a verified state blob, and the source renders from it
(02 §7). The source may be an instrument or an effect; an effect source renders the effect probe
signals (§5) instead of the note plan.

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

Effect plugins need an input signal. Four cases:

| Case | Input | Target |
| --- | --- | --- |
| Reamp | the user's dry WAV | the user's processed WAV (best case: exact) |
| Known source | the built-in probe signals below | processed version of the same signals |
| Effect to effect | the built-in probe signals | the source effect's output for each signal (§2) |
| Unknown dry | none | statistics only (tail length, spectral tilt, modulation depth): weak, flagged |

**Probe signals** [D], generated in code (no audio assets), deterministic, each chosen for what it
reveals:

| Signal | Reveals |
| --- | --- |
| Impulse (plus silence for the tail) | linear response, reverb and delay tails |
| Log sine sweep 20 Hz-20 kHz, 4 s | frequency response; harmonic distortion separated by the sweep's timing |
| Sine 1 kHz and 100 Hz at stepped levels (-48 to 0 dBFS in 6 dB steps) | distortion against drive, compressor curve, gating |
| Level step (-30 to -6 dBFS) on a sustained tone | attack and release of dynamics |
| Sustained saw chord, 4 s | chorus, phaser, flanger, tremolo rate and depth |
| Pink noise bursts (short and long) | time-varying filters, envelope followers |
| Synthetic drum hit and a short loop (kick, snare, hats from noise and sines) | transient handling, a musical check for the ear (`ab.wav`) |
| Silence | self-noise, hum, self-oscillation, drones |

The default effect plan (`plans/effect_default.json`) uses a subset (impulse, sweep, two sine levels,
chord, drum loop) and holds out one level and one signal for the holdout check (07 §2); the full set is
for the probe (04 §3) and the doctor (08). Signals are rendered at the input level the plan states, so
a level-dependent effect is fitted at the levels it will see.

Tail length (reverb, delay) comes from the plugin's tail report (CLAP `clap.tail`, VST3
`getTailSamples`) and from the target's decay, and long tails make renders slow, so effect stages are
last (04 §4). The plugin's reported latency is removed from the output before scoring (02 §5).

## 6. Preset harvest (`mimicry harvest`, `mimicry capture`) [A]

There is no common standard for how a plugin stores or changes presets, and no guarantee that it
supports program changes at all. The harvest therefore tries the common methods in order, verifies
each loaded preset, reports what failed, and falls back to the user picking presets by hand.

**Ladder** (02 §7 for the calls). Each step runs until it yields presets; `--method` forces one:

1. Preset files the user names (`--from <file or folder>`): `.vstpreset`, CLAP state files,
   `params.json`.
2. The plugin's own catalogue: CLAP preset discovery + preset-load; VST3 `IProgramListData`.
3. VST3 program lists through the program-change parameter.
4. MIDI program change (CLAP MIDI on a note port; VST3 `IMidiMapping`), banks of 128 with bank select
   when the plugin maps it.
5. Heuristic: a stepped parameter named Program / Preset / Patch, stepped through its values.
6. Manual capture (below).

**Verification** of every preset (a failed preset load is information, CLAUDE.md rule 6): the state
hash differs from the defaults and from the previous preset; the render (note probe plan, or effect
probe signals) differs from the default render by more than the repeatability floor; two renders after
`load_state` agree within the floor (the blob alone reproduces the sound). Outcomes per preset:
`ok`, `no-change`, `duplicate` (same state hash or same render as an earlier preset), `not-reproducible`,
`load-error`, `timeout`. A method whose first presets are all `no-change` is abandoned and the report
says so.

**Manual capture** (`mimicry capture <plugin>`): the worker opens the plugin's editor in its own window
(02 §3); the user browses presets in the plugin's own browser; a key or the small capture window saves
the current state, parameter snapshot and a name (the plugin's program name when it has one, else typed
or `capture-NNN`, plan OQ14). **Watch mode** (`--watch`) captures by itself when the plugin signals a
preset change: a burst of parameter changes within 50 ms [D], a VST3 `restartComponent` with
`kParamValuesChanged`, a CLAP parameter rescan of values, or a changed state hash polled every 500 ms
[D] for plugins whose presets change hidden state only (plan OQ13). Each capture is verified and
deduplicated as above, so the user can click through a whole bank with the plugin's own "next" arrow.

**Recipe**: the method that worked, with its details (program-change parameter id and count, MIDI bank
layout, wait times), is stored in the plugin profile (01 §7), so the next harvest of the same plugin
goes straight to it. Recipes are plain JSON and can be shared. Learning the editor's own "next preset"
click and replaying it (plan WP7.5) would close the loop for plugins with no other method.

**Output**: `presets/<format>-<uid>-<hash>/` holds, per preset, `NNN-<name>.state`,
`NNN-<name>.params.json`, and an `index.json` (name, method, ref, state hash, outcome), plus
`harvest-report.csv`. Nothing is written into the plugin's own preset folders (05 §8).

## 7. Least-error transfer [A]

The end goal of plugin-to-plugin work is to use the presets of one synth or effect on another with the
least error the target allows. Rules:

- Each preset's result is judged against the target's repeatability floor and the source's: fit loss,
  holdout loss, and per-feature errors (what is missing: brightness, envelope, a modulation, a tail).
- A preset that stays above 1.5x the floor after the full pipeline is reported as **unmatched**, with
  the feature that dominates the residual as the likely reason (§2).
- `transfer --source A --target B --target C` ranks candidate targets for a source bank by median and
  worst loss, so the user can choose the plugin that covers the bank best.
- Results carry the source preset's name, and the map (04 §8) learned over the bank is written with
  them.
