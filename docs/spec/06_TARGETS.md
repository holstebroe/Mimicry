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

1. Preset files and folders: the user's (`--from <file or folder>`: `.vstpreset`, CLAP state files,
   `params.json`), the file locations and extensions a CLAP plugin declares through preset discovery,
   and the standard VST3 preset folders for the plugin's vendor and name. Many plugins keep presets in
   their own folder in their own format; with `--raw-files` such files are tried as state blobs, which
   works only where the file is the plugin's state, and the verification below decides [I].
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

Implemented [A] (WP5.8, `targets/Harvest`, one worker): the verification render is one note (C3, 600 ms,
velocity 100; `--note`, `--gate`) at the profile's settle time. "Differs" means a render difference of at
least `max(2 × floor, 0.15)` [D: as the probe], where the difference is the larger of the embedding
distance (04 §7) and the level difference in dB, and the floor is the difference between two renders of
the defaults. The default state is saved after rendering the defaults [I]; since a coordinate round trip
need not be bit-exact, a preset with the defaults' sound but not their exact state is still `no-change`.
Order of checks: state equals the defaults → `no-change`; same state hash as an earlier `ok` preset →
`duplicate`; the loaded state does not render → `load-error`; sound like the defaults → `no-change`; sound
like an earlier preset → `duplicate`; a second render differs → `not-reproducible`; else `ok`, with a
`params_reproduce` flag saying whether a render from the parameter snapshot alone matches (it does not for
plugins with hidden state). Duplicates are checked across methods, so `--all-methods` collects each sound
once. A method is abandoned when its first 3 presets are all `no-change` or failed [D]; MIDI program change,
which cannot say how many programs exist, stops after 8 empty slots in a row [D] ("end of the bank"),
while VST3 program lists and program data report their program count and are tried slot by slot to the
end, empty slots included [A]. A state equal to the defaults or to an earlier `ok` preset is classified
without a render. The defaults must render twice; a failed second render fails the harvest rather than
setting a meaningless floor [A]. A failed preset listing (CLAP discovery, VST3 program lists) is reported
on the console and as `listing_error` in `index.json`; file presets are still tried [A]. `--method` forces one method, `--all-methods` runs every method,
`--max N` stops after N presets, `--wait-ms` sets the wait after each load (200 ms [D]). Files: `--from`
files are tried whatever their extension; in `--from` folders `.state` (and `.vstpreset` for VST3), or
every file with `--raw-files`; for VST3 also the standard folders (`~/.vst3/presets`, `/usr/share/vst3/presets`,
`/usr/local/share/vst3/presets` on Linux; `~/Library/Audio/Presets`, `/Library/Audio/Presets` on macOS;
`Documents\VST3 Presets`, `%APPDATA%\VST3 Presets`, `%PROGRAMDATA%\VST3 Presets` on Windows, each
`/<vendor>/<plugin>`) [S: VST3 SDK]. A `params.json` as a preset source is not implemented yet. Effects
are verified like instruments until the effect probe signals exist (WP5.7).

**Names** come from the plugin where a format offers them: CLAP discovery metadata; VST3 program names
(`IUnitInfo::getProgramName`) and the program-change parameter's value text; the file name for file
presets. Otherwise a preset is `capture-NNN` (or `<method>-NNN`), and the user may rename it in
`index.json` [D: owner, plan OQ14].

**Manual capture** (`mimicry capture <plugin>`, **Windows only**, 02 §3): the worker opens the plugin's
editor in its own window; the user browses presets in the plugin's own browser; a key or the small
capture window saves the current state, parameter snapshot and a name. **Watch mode** (`--watch`)
captures by itself when the plugin signals a preset change: CLAP `preset-load.loaded` called by the
plugin (it also names the preset through the discovery index), an edit of the VST3 program-change
parameter (its value text names the program), a burst of parameter changes within 50 ms [D], a VST3
`restartComponent` with `kParamValuesChanged`, a CLAP parameter rescan of values, or a changed state
hash polled every 500 ms [D] for plugins whose presets change hidden state only (plan OQ13). Each
capture is verified and deduplicated as above, so the user can click through a whole bank with the
plugin's own "next" arrow.

**Taught click** (`mimicry capture --teach`, Windows) [A]: the user presses the plugin's own "next
preset" button once. The worker records the click from the editor window's message hook: the child
window under the cursor and the position relative to it, so it survives the window being moved. The
click must change the state hash, else the user is asked to try again. Mimicry then replays it as posted
`WM_LBUTTONDOWN`/`WM_LBUTTONUP` messages to that child window (no real mouse movement, no focus needed),
or with `SendInput` on the editor brought to the front for plugins that ignore posted messages. After
each click it waits for the change signal or a settle time, captures and verifies. It stops when a state
hash repeats (the bank wrapped around), when 3 clicks in a row change nothing [D], or at `--max`. The
click and its wait time are stored in the recipe.

**Recipe**: the method that worked, with its details (program-change parameter id and count, MIDI bank
layout, wait times), is stored in the plugin profile (01 §7), so the next harvest of the same plugin
goes straight to it, a taught click included. Recipes are plain JSON and can be shared. Implemented [A]
(WP5.10): `harvest_recipe` in the profile (method, the program parameter or list id of its first ok preset, presets offered and
found, wait time), written by the first method that yields presets and kept by `probe` and `fit`; the
next harvest tries that method first (that parameter or list first) and stops there if it yields
presets; `--no-recipe` ignores it.

**Output**: `presets/<format>-<uid>-<hash>/` holds, per preset, `NNN-<name>.state`,
`NNN-<name>.params.json`, and an `index.json` (name, method, ref, state hash, outcome), plus
`harvest-report.csv`. Nothing is written into the plugin's own preset folders (05 §8). Implemented [A]:
files only for `ok` presets (numbered 1..N in harvest order), every tried preset in `index.json` and the
report (number, name, method, index, outcome, duplicate of, difference, params reproduce, state hash,
message), the per-method summary and the recipe in `index.json`; a re-harvest into the same folder first
removes the files the previous `index.json` listed, and nothing else.

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
