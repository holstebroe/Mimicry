# 07 — Output and validation

## 1. Run output

Each run writes `runs/<timestamp>-<plugin>-<target>/` [S: Acidus `calibration_results/<timestamp>/`
and `test/resources/<src>/stage<N>/`]:

| File | Content |
| --- | --- |
| `best.state` | The plugin's own state (CLAP state blob; VST3 component + controller state) |
| `best.vstpreset` | VST3 targets only: the same state as a `.vstpreset` file (§3) |
| `best.params.json` | Format, plugin uid and version hash, every parameter (id, name, module, normalised and displayed value), note plan, per-feature errors, gain |
| `top-k/` | The diverse alternatives (state + params + audio) |
| `match.wav` | The match rendered through the target's note plan |
| `ab.wav` | Target, 0.3 s of silence, match; level-matched by the solved gain, same scale for both [S: Acidus `ab.wav`] |
| `report.md` | Per-stage table (before → after), parameters at a bound, worst frames, failures and learned constraints, repeatability floor, holdout result, time and evaluations, host format and processing mode |
| `diff.ppm` | Spectrogram of target, match and difference (PPM, as Tannhauser's GUI snapshots [S]) |
| `result.json` | Everything machine-readable |

Always listen to `ab.wav`: the score smooths over narrow features that matter by ear [S].

## 2. Generalisation checks

- **Holdout**: render the match at keys, velocities and gates that were not fitted and score them
  against the same holdout items of the target (plugin-to-plugin) or against the plugin's own
  behaviour across keys (WAV). Holdout loss above 2x fit loss prints an overfit warning.
- **`mimicry eval`**: score any state without fitting [S: `--evaluate-only`], including on notes
  the fit never saw.
- **Parameters at a bound** are listed: they usually mean the plugin cannot reach the target (or is
  compensating for something) [S: Acidus report].

## 3. Preset export

Every run writes `best.state` and `best.params.json`.

- **CLAP targets**: `best.state` loads wherever the plugin's own state loads; the way to apply it in a
  DAW depends on the host (load plugin state, or the plugin's preset import). `best.params.json` is the
  portable form. Plugins with a parameter-file import (as Acidus's IMPORT CALIBRATION) can use it
  directly.
- **VST3 targets**: Mimicry also writes a **`.vstpreset`** file, the format hosts load natively. It holds
  the component state and the controller state in the format's chunk layout (header, chunks, chunk list)
  as documented in the VST3 SDK; the class id is the plugin's. The file is verified by loading it back
  through the host and comparing a render (T14), and by the SDK's validator where available. For
  clap-wrapper plugins (all of ours), the `.vstpreset` holds the wrapper's state, which round-trips
  within the same wrapper version.
- A match for a plugin in one format cannot be reused as a state blob in the other format (the
  blobs differ), but both can be produced by running the fit against each build.

## 4. Tests

All tests are executables run by `ctest`; DSP-style tests return non-zero on failure as in
`tannhauser_dsp_test` [S].

| ID | Test |
| --- | --- |
| T1 | Host loads Acidus, Tannhauser, Tearwash and `mimicry_test_synth` as CLAP, and Acidus (`acidus_vst3` [S]) and `mimicry_test_synth_vst3` as VST3; instantiates, renders a note, destroys |
| T2 | Parameter enumeration equals the plugin's own list in both formats; units parsed from value text |
| T3 | Repeatability: same job twice gives identical audio for deterministic plugins; floor measured for others |
| T4 | Feature invariance: gain change shifts only `gain_db`; time shift shifts only onset; phase change leaves `msstft`, `harm`, `bands` unchanged |
| T5 | CMA-ES and sep-CMA-ES reach known optima on sphere, Rosenbrock, Rastrigin (with restarts) |
| T6 | Round trip (`rehearse`): 20 random presets of a test plugin, in each format; recovered loss under 1.5x floor in >= 90 % and normalised parameter error reported per parameter group |
| T7 | Isolation: 1000 planted crashes/hangs, in each format; supervisor survives, no leaked handles, shared memory or zombies |
| T8 | Bisection finds the planted constraint (`A > 0.8 && B > 0.9`) with thresholds within 2 %, in each format |
| T9 | Poisoned state: after a NaN-poisoning combination, the next valid render is correct (canary), in each format |
| T10 | Output: `ab.wav` layout and levels, `best.state` reloads and reproduces the match, `diff.ppm` valid |
| T11 | Plugin-to-plugin: Acidus calibration build as source and target with different calibration profiles; recover the profile within tolerance (the Acidus `--rehearse` idea [S]) |
| T12 | Throughput: renders/s on the reference machine, per format, reported (target: >= 50 evals/s for a light synth on 8 cores [D]) |
| T13 | **Backend equivalence**: `mimicry_test_synth` as CLAP and as VST3 gives the same parameters (ids, names, units) and renders within the repeatability floor for the same musical settings |
| T14 | `.vstpreset` round trip: write, load back through the VST3 backend, render, equal to the original render; SDK validator passes on `mimicry_test_synth_vst3` |
| T15 | **Cross-format transfer**: Acidus as CLAP source, `acidus_vst3` as target (the same engine through the wrapper): the optimizer recovers the source parameters within tolerance, and the reverse |
| T16 | VST3 hosting specifics: restart request handling, program-change pinning, offline mode set, tuning cents honoured on `NoteOnEvent` |

## 5. `mimicry_test_synth`

A deliberately small synth built with the project as a **CLAP** plugin and, through clap-wrapper like
the other plugins, as a **VST3** plugin (so both host backends are tested against the same code). It has
a sine/saw oscillator, a one-pole filter and an envelope, with **planted hazards** switchable by
parameter, used by T6-T9: a NaN above a threshold; a null dereference when two parameters are both high;
a sleep (hang) at one combination; a filter state that stays poisoned after a bad combination; a random
LFO phase (repeatability floor) with a seed parameter; a program-change parameter and a parameter that
requests a component restart (for T16). It is also the first thing the host is developed against.

## 6. Pipeline noise floor

Rendering a preset twice and scoring the pair gives the floor of the whole pipeline [S: Acidus self-test
scored its own render against itself at a weighted error of 0.45, with 98 % of harmonics within 3 dB].
What a real fit leaves is the difference between the plugin and the target on top of that floor.
