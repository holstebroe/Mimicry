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

Implemented [A] (WP6.1, 6.5): `report.md` has the status (and how often the run was resumed), plugin and
target, result, repeatability floor, pitch method, the stage table, the loss of each feature for the best
candidate, **parameters at a bound** of their searched range (within 1 % of the safe box), the outcome counts and
the learned hazards. `diff.ppm` is three log-frequency spectrograms stacked (target, match at the solved gain,
difference in dB: blue where the match is quieter, red louder, faded below the noise floor), 480 columns by
160 rows each; `diff.bmp` is the same image as a 24-bit BMP for viewers that do not read PPM. `best.vstpreset` is written for VST3 targets. A run that was cancelled writes all of these too
(`"status": "cancelled"` in `result.json`), plus `checkpoint.json` and `run.json` (09 §3). `mimicry eval --plugin P
--state S --wav T.wav` scores a `best.state` or a `.vstpreset` without fitting (gain solved, onset offset
estimated; per-feature losses; `--json`).

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
- **VST3 targets** (`output/VstPreset`): Mimicry also writes a **`.vstpreset`** file, the format hosts load natively. It holds
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
| T9 | Poisoned state: after a NaN-poisoning combination, the next valid render is correct (canary), in each format; poison that survives reset and state load fails the canary and restarts the worker |
| T10 | Output: `ab.wav` layout and levels, `best.state` reloads and reproduces the match, `diff.ppm` valid |
| T11 | Plugin-to-plugin: Acidus calibration build as source and target with different calibration profiles; recover the profile within tolerance (the Acidus `--rehearse` idea [S]) |
| T12 | Throughput: renders/s on the reference machine, per format, reported (target: >= 50 evals/s for a light synth on 8 cores [D]) |
| T13 | **Backend equivalence**: `mimicry_test_synth` as CLAP and as VST3 gives the same parameters (ids, names, units) and renders within the repeatability floor for the same musical settings |
| T14 | `.vstpreset` round trip: write, load back through the VST3 backend, render, equal to the original render; SDK validator passes on `mimicry_test_synth_vst3` |
| T15 | **Cross-format transfer**: Acidus as CLAP source, `acidus_vst3` as target (the same engine through the wrapper): the optimizer recovers the source parameters within tolerance, and the reverse |
| T16 | VST3 hosting specifics: restart request handling, program-change pinning, offline mode set, tuning cents honoured on `NoteOnEvent` (run for both formats in `host_*`; table changes after a restart in `worker_*`) |
| T17 | **Harvest**: the test synth's factory presets found and loaded through CLAP preset discovery, VST3 program data, the VST3 program list and MIDI program change; every preset `ok`, the planted duplicate and the planted no-op preset reported as such; the recipe is stored and reused; source renders from the captured state equal renders from the preset's parameters. Implemented in `host_*` (every method in-process, file location, state files, preset-load errors) and `harvest_clap`/`harvest_vst3` (through a worker: per method, ladder stop, recipe first, library files and report, library states reloaded as file presets, state render through shared memory, `mimicry harvest` twice with the recipe); VST3 program data has no fixture (clap-wrapper lacks it) |
| T18 | **Effects**: `mimicry_test_fx` (CLAP and VST3) renders the probe signals with input and latency compensation in both formats; reamp fit and effect-to-effect transfer recover random presets within 1.5x the floor; effect features pass their T4 cases |
| T19 | **Map**: test synth CLAP → test synth VST3 maps every parameter to itself with full confidence; against a renamed and re-tapered variant, roles and laws are recovered within tolerance |
| T21 | **Cancel and resume**: see 09 §6 (`opt`, `cli_clap`, `cli_vst3`, `cli_clap_to_vst3`); harvest cancel and `--continue` in `harvest_*` |
| T22 | **Front end logic**: see 09 §6 (`gui_core`, `gui_e2e_clap`) |
| T20 | **Doctor**: report written for the test synth and the test effect in both formats, `report.json` numbers equal the probe's, the planted hazards appear in the health section; the frequency response of the test effect's filter matches its analytic response within 0.5 dB |

## 5. `mimicry_test_synth`

A deliberately small synth built with the project as a **CLAP** plugin and, through clap-wrapper like
the other plugins, as a **VST3** plugin (so both host backends are tested against the same code). It has
a sine/saw oscillator, a one-pole filter and an envelope, with **planted hazards** switchable by
parameter, used by T6-T9: a NaN above a threshold; a null dereference when two parameters are both high;
a sleep (hang) at one combination; a filter state that stays poisoned after a bad combination; a random
LFO phase (repeatability floor) with a seed parameter; a program-change parameter and a parameter that
requests a component restart (for T16). It is also the first thing the host is developed against.
Three more hazards test spec 05 §5 and §9 [A]: *Hazard Sticky* (NaN for the life of the instance,
surviving reset, re-activation and state load, so only the canary's worker restart clears it), *Hazard
Throw* (`process` throws a C++ exception) and the hidden *Hazard Rename* (renames *Hidden Drive* and asks
for a parameter rescan, so the parameter table changes mid-run). They are appended to the table, so no
earlier id moves. For pitch (02 §5) it takes MIDI pitch bend on a note port with the MIDI dialect, and two
hidden parameters: *Ignore Tuning* (tuning has no effect, so only bend moves the pitch) and *Bend Range*
(1-24 semitones, default 2) [A]. A hidden *Require Offline* switch outputs NaN unless the host set
offline rendering (CLAP `render` extension, VST3 `kOffline` passed on by clap-wrapper): T16's "offline
mode set" check [A]. A hidden *Smoothing* parameter (ms, default 0) glides Level and Cutoff from their
defaults with a one-pole after every activation, for the settle-time probe (05 §6) [A]. The VST3 build therefore carries clap-wrapper's MIDI-mapping proxies (left
out of the table, 02 §4) and its MIDI program-change parameter (pinned, never sent).

For T17 it exposes a small factory bank (CLAP preset discovery, VST3 program list with program data,
MIDI program change), including one duplicate and one preset that changes nothing. Implemented [A]: six
presets (*Init* = the defaults, the no-op; *Soft Pad*, *Lead*, *Pluck*, *Lead Copy* = *Lead*, the duplicate;
*Vibrato*), each replacing every value. CLAP: a preset-discovery provider with the bank as a plugin
location (load key = index) and, when `MIMICRY_TEST_PRESET_DIR` is set, that folder as a file location of
`.mtspreset` state files; `preset-load` defers the load to `on_main_thread` and then calls the host's
`loaded` (or `on_error` for an unknown key), so the host's wait is exercised; MIDI program change selects
the bank. VST3: clap-wrapper turns its own 128-entry program list ("Program N") behind a program-change
parameter into MIDI program change, so the VST3 build is harvested through the program list; clap-wrapper
has no `IProgramListData`, so the program-data path has no fixture yet. The read-only *Meter* is saved at
its default, so a state does not depend on the last block played. **`mimicry_test_fx`**
[A] is its effect sibling for T18 and T20: a filter, a waveshaper, a compressor, a short delay with
feedback and an LFO-modulated gain, built the same two ways, with reported latency.

## 6. Pipeline noise floor

Rendering a preset twice and scoring the pair gives the floor of the whole pipeline [S: Acidus self-test
scored its own render against itself at a weighted error of 0.45, with 98 % of harmonics within 3 dB].
What a real fit leaves is the difference between the plugin and the target on top of that floor.
