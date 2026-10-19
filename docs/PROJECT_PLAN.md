# Mimicry — Project implementation plan

Living document, same style as Tannhauser's `docs/PROJECT_PLAN.md`. Pick up any open issue from the
spec in `spec/`; update the status column and the log when an issue changes state.

Status: ✅ done · 🟡 partial / first version · ⬜ open · 🔬 needs measurement or a decision

**Goal 1:** `mimicry fit` matches a WAV with a **CLAP or VST3** instrument, safely, offline, with a preset
and A/B audio out. **Goal 2:** plugin-to-plugin transfer over a whole bank, in any format combination
(CLAP→VST3, VST3→CLAP, same format). **Goal 3:** image targets.

Idea origin: a tool that hosts a plugin, takes a sample, and uses a non-linear optimizer to build a
preset that matches it; variants take an image, or a source plugin's presets with a target plugin.
Optimizer techniques come from Acidus (`tools/calibrate_reference.py`, `tools/fit_stage.py`).

## Document map

| Doc | Content |
| --- | --- |
| `spec/01_ARCHITECTURE.md` | Product shape, formats, constraints (CMake, C++20), processes, layout, commands, plugin profile |
| `spec/02_HOSTING.md` | `PluginHost` interface, CLAP and VST3 backends, concept mapping, threads, parameters, render loop, state, build |
| `spec/03_FEATURES_LOSS.md` | Features, weights, level, noise floor, cost |
| `spec/04_OPTIMIZER.md` | Encoding, semantics, probe, stage pipeline, CMA-ES, diversity, fingerprint, mapping |
| `spec/05_RESILIENCE.md` | Isolation, outcomes, search under failure, failure bisection, state poisoning, format-specific notes |
| `spec/06_TARGETS.md` | WAV, plugin-to-plugin (cross-format), image, effects |
| `spec/07_OUTPUT_VALIDATION.md` | Run output, `.vstpreset` export, generalisation checks, tests T1-T16, test synth |

## Build and run

```
git submodule update --init                       # clap
cmake -B build -DCMAKE_BUILD_TYPE=Release         # VST3 SDK fetched, or -DVST3_SDK_ROOT=... 
cmake --build build -j
ctest --test-dir build
build/mimicry list path/to/plugin.clap            # or plugin.vst3: parameters, semantics, search roles
build/mimicry probe path/to/plugin.clap           # writes profiles/<format>-<uid>-<hash>.json
build/mimicry fingerprint path/to/plugin.clap     # optional: random-preset database for S1 seeding
build/mimicry fit --plugin path/to/plugin.vst3 --wav target.wav --minutes 20
```

## Work packages

### WP0 — Bootstrap
| ID | Issue | Status |
| --- | --- | --- |
| 0.1 | CMake project, C++20, `clap` submodule, VST3 SDK fetch / `VST3_SDK_ROOT`, `MIMICRY_VST3_HOSTING` option, IPO in Release, CI (Linux/Windows/macOS) | 🟡 builds and tests on Linux/GCC 13 (VST3 SDK v3.8.0 fetched by `cmake/Vst3Sdk.cmake`); Windows/macOS CI unverified until first push |
| 0.2 | `CLAUDE.md`, evidence-tag conventions | ✅ |
| 0.3 | Core utilities: Json, Wav, Fft, Rng | ✅ `src/core`, tested by `mimicry_core_test` |

### WP1 — Host and worker (spec 02, 05 §1)
| ID | Issue | Status |
| --- | --- | --- |
| 1.1 | `PluginHost` interface, `ParamDesc`, `Job`, outcome codes, format selection by extension | ✅ interface in `PluginHost.hpp`; backends implement `Engine` (`Engine.hpp`) |
| 1.2 | CLAP backend: loader, host API, thread-check, instance lifecycle | ✅ `host/clap/ClapEngine.cpp` (params, state, tail, latency, note expression tuning, log, thread-check); Windows/macOS loader paths unverified |
| 1.3 | Parameter enumeration with units from value text (both backends) | ✅ `ParamUtil`; texts at 0/25/50/75/100 %, units, log decision, program-change heuristic |
| 1.4 | Shared render core: events, settle, notes, tail, output checks | ✅ `host/Renderer.cpp` (reset per render, pinned bypass/program, checks of spec 05 section 2, restart handling) |
| 1.5 | Worker process, shared-memory audio, supervisor pool, watchdog | 🟡 POSIX done and tested (`WorkerClient`, `WorkerPool`, recycling, T7 with 300 crashes + hangs); pool workers now live on long-lived threads (a respawn from a short-lived thread was killed by `PR_SET_PDEATHSIG`, fixed in WP3); Windows returns an error |
| 1.6 | `mimicry_test_synth` (CLAP) and `mimicry_test_synth_vst3` (clap-wrapper) with planted hazards | ✅ both; the VST3 build is a separate project (`src/tests/test_synth_vst3`) via ExternalProject. Hazards: NaN, null deref (A>0.8 && B>0.9), hang, poison, random LFO phase, program, restart. Open: T16-style checks for the restart/program parameters |
| 1.7 | **VST3 backend**: SDK hosting module loading, component + controller + connection, host classes, `ProcessData`, parameter changes, event list, offline setup | ✅ `host/vst3/Vst3Engine.cpp` on the SDK hosting classes; T13 equivalence passes against the clap-wrapper build. Real third-party VST3 plugins not yet tried (OQ5) |
| 1.8 | VST3 state (component + controller), program lists, program-change pinning, restart requests, tail/latency | 🟡 state, pinning (flag or name heuristic), restart/rescan, tail, latency done; program lists (`IUnitInfo`) open |
| 1.9 | Main-thread message pump in the worker (Win32 / macOS / Linux) | 🟡 `MessagePump` + 10 ms idle pump in the worker; Win32 and macOS run-loop code written but unbuilt/unverified |

### WP2 — Features and loss (spec 03)
| ID | Issue | Status |
| --- | --- | --- |
| 2.1 | `msstft`, `env`, `bands`, noise-floor clamp, closed-form gain | ✅ `src/features` (`Features`, `Loss`, `Preprocess`); T4 invariance (gain, phase, polarity, time shift) in `mimicry_features_test`. Cost is ~27 ms per candidate for a 2 s note, not the 5 ms target of spec 03 §7 (OQ2) |
| 2.2 | f0 estimation and pitch application (VST3 tuning cents / CLAP note expression / bend calibration) | 🟡 `Pitch`: comb search + LS refinement, key/cents, `--note` parsing; `Target` turns it into the note key and cents. Tuning cents and nearest-key fallback were already in the backends (1.2, 1.7); MIDI pitch-bend calibration and the residual report are open |
| 2.3 | `harm`, `inter`, `harmt` for pitched targets (port from Acidus) | ✅ written from the spec definitions (Acidus source is not in this repository, so the evidence tags stay [S] only where the spec says so); one-sided `inter` against the target's own gap level |
| 2.4 | `shape` features, weight presets by sample kind | ✅ `shape` (centroid, flatness, rolloff, flux, MFCC 1-13), `Weights::preset`, `--w-<feature>` override (`Weights::set`), kind classifier with `--kind` override. Thresholds are [D] guesses (OQ3) |
| 2.5 | Repeatability floor, render cache | ✅ `repeatability_floor_db`, `repeat_policy` (renders 1-4, success threshold 1.5x), `opt::RenderCache` (exact key, LRU, failures cached except timeout/host error); wired into `AudioObjective` (cache + renders per candidate from the profile floor); seed/random parameters pinned by name in `Space` |

### WP3 — Optimizer (spec 04)
| ID | Issue | Status |
| --- | --- | --- |
| 3.1 | CMA-ES port from Acidus with restarts, Jacobi eigensolver; sep-CMA-ES | ✅ `core/Cma` (full + separable, restarts from the best with doubled population); T5 in `mimicry_opt_test` (sphere, Rosenbrock, Rastrigin with restarts, sep-CMA-ES ellipsoid 100D / sphere 200D). Written from the spec: the Acidus source is not in this repository |
| 3.2 | Unit-box encoding: CLAP plain values with log/linear decode, VST3 native normalised coordinate (02 §1), stepped handling, boundary penalty | ✅ `opt/Space`: roles (free / stepped / categorical / fixed), safe box clip + boundary penalty, grid snap, probe-driven log warp for both formats (04 §9, OQ9), pitch pinning when S0 applies pitch |
| 3.3 | Unsupervised pipeline (S1, S5, S7), time cap, patience | ✅ `opt/Pipeline` + `opt/Objective` (`AudioObjective`: batch render through a `RenderBatch`, cache, parallel scoring); `mimicry fit` drives it. Test synth via worker pool recovers a preset in both formats (`fit_clap`, `fit_vst3`: < 0.5 dB at 4000 evaluations) |
| 3.4 | Semantics classifier and staged hybrid pipeline | ✅ `core/ParamSemantics` (token priors + confidence, output level, seed, pitch names); S2-S4 by group; hybrid takes the probe's class; interacting pairs share a stage; categorical sweep after S1 and each group stage |
| 3.5 | Sensitivity probe, dead-parameter detection, pair probes, plugin profile (per format) | ✅ `opt/Probe` + `opt/Profile` (`mimicry probe`): classes, response shape, safe box, 2x2 interaction; profile keyed on format + uid + version hash, entries on id + name. Thresholds are [D] (OQ3). Settle-time probe (05 §6) open |
| 3.6 | Categorical outer loop; R restarts; top-K diversity | ✅ S6 (importance, enumeration of <= 3 x 4 values, inner S5), R independent runs, top-K by parameter and embedding distance (`opt/Embedding`) |
| 3.7 | Fingerprint database and nearest-neighbour seeding | ✅ `opt/Fingerprint` (`mimicry fingerprint`, binary file, exact parameter-table match); `fit` seeds S1 with the 8 nearest neighbours |
| 3.8 | Surrogate / Bayesian optimisation for slow plugins | 🟡 `opt/Surrogate`: GP pre-screening of 3λ CMA-ES samples (auto above 1 s per candidate). Not yet measured on a slow real plugin |

### WP4 — Resilience (spec 05)
| ID | Issue | Status |
| --- | --- | --- |
| 4.1 | Ranking of invalid candidates, resample, sigma adaptation | 🟡 ranking (severity, then distance to the best valid point) and sigma shrink + recentre in `Pipeline` (WP3); resampling against learned constraints needs 4.3 |
| 4.2 | Safe box from probe | 🟡 probe shortens each range to the last good probe value; `Space` clips to it. Joint (multi-parameter) safety is 4.3 |
| 4.3 | Failure bisection and constraint store | ⬜ |
| 4.4 | State reset, canary, instance recycling (both formats) | ⬜ |
| 4.5 | Failure reports and `mimicry replay` | ⬜ |
| 4.6 | Format-specific handling (05 §9): GUI-expecting VST3 plugins, restart requests, exceptions | ⬜ |

### WP5 — Targets (spec 06)
| ID | Issue | Status |
| --- | --- | --- |
| 5.1 | WAV target analysis and `mimicry fit` | 🟡 `mimicry fit` runs end to end (analysis, pitch to the note, profile, fingerprints, pipeline, run directory). Open: `--midi`, `--gate` defaults from the envelope, `--velocity` |
| 5.2 | Probe plans, source rendering and cache | ⬜ |
| 5.3 | `mimicry transfer` for one preset, then the bank loop and report, in any format combination | ⬜ |
| 5.4 | Learned mapping `knob_map.json` (keyed by source and target format + uid) | ⬜ |
| 5.5 | Image Route A (descriptors) and `image_map.json` | ⬜ |
| 5.6 | Image Route B (spectrogram) | ⬜ |
| 5.7 | Effect plugins: reamp, known source | ⬜ |
| 5.8 | Preset sources: `.vstpreset`, VST3 program lists, CLAP preset-load/discovery, state files, folders | ⬜ |

### WP6 — Output and tests (spec 07)
| ID | Issue | Status |
| --- | --- | --- |
| 6.1 | `best.state`, `best.params.json`, `match.wav`, `ab.wav`, `report.md`, `diff.ppm` | 🟡 `fit` writes `best.params.json`, `best.state`, `match.wav`, `ab.wav`, `result.json`, `top-k/`; `report.md`, `diff.ppm` and `.vstpreset` open |
| 6.2 | Holdout and `mimicry eval` | ⬜ |
| 6.3 | Tests T1-T12 | 🟡 smoke, core, host (T1 T2 T3 T9 partly), worker (T1 via worker, T7, T9), T13 equivalence, T5 (`opt`), optimizer against the test synth in both formats (`fit_clap`, `fit_vst3`, a small T6) |
| 6.4 | `mimicry rehearse` round trip | ⬜ |
| 6.5 | `.vstpreset` writer and round-trip test (T14) | ⬜ |
| 6.6 | Backend equivalence, cross-format transfer, VST3 specifics (T13, T15, T16) | 🟡 T13 done; T15, T16 open |

### WP7 — Later
| ID | Issue | Status |
| --- | --- | --- |
| 7.1 | Software-rendered GUI (spectrogram A/B, progress, parameter table), glue from Acidus | ⬜ |
| 7.2 | Mimicry Capture plugin (CLAP + VST3 via clap-wrapper) | ⬜ |
| 7.3 | Stereo features | ⬜ |
| 7.4 | Further backends (AU, LV2) if wanted; VST2 is out of scope | ⬜ |

## Suggested order

WP0 → WP1.1-1.6 (interface, CLAP backend, test synth in both builds; everything is developed against
it) → WP2.1/2.5 → WP3.1-3.3 (first working `fit` on CLAP, no names, no probe) → WP6.1 → **WP1.7-1.9
(VST3 backend, T13)** → WP4.1 → WP3.4-3.6 → WP4.2-4.6 → WP5.2-5.4, 5.8 → WP6.5-6.6 → WP5.5-5.6 → WP7.

The VST3 backend lands before the resilience work so failure handling, probes and plugin profiles are
built and tested against both formats from the start.

## Open questions

| ID | Question | Status |
| --- | --- | --- |
| OQ1 | Dependencies: own PNG/JPEG decoder for image targets, or allow a public-domain single header (stb_image)? Own JSON stays the default | 🔬 |
| OQ2 | Performance target on the real machine (evals/s for Acidus, Tannhauser); measure before tuning weights. First number: test synth, 1 s note, 3 workers on 4 cores: ~90 evaluations/s (features dominate) | 🔬 |
| OQ3 | Default weights and probe plan are guesses; tune on real samples and on Acidus/Tannhauser presets | 🔬 |
| OQ4 | Resolved: `.vstpreset` export is in scope (WP5.8, WP6.5). Open detail: wrapper-state round trip across clap-wrapper versions | 🔬 |
| OQ5 | Plugins that need a message loop, window or licence dialog to initialise (mostly third-party VST3): how far does the minimal main-thread pump get us? Measure on real plugins (WP1.9, WP4.6) | 🔬 |
| OQ6 | Whether `ImageFeatures` is extracted into a shared header with the image-seeded synth | 🔬 |
| OQ7 | Resolved for GCC 13: SDK v3.8.0 is MIT licensed and its hosting sources build under C++20. Open: MSVC and AppleClang (CI) | 🔬 |
| OQ8 | VST3 offline processing mode vs realtime: how many real plugins sound different, and should the fit offer `--realtime-mode`? | 🔬 |
| OQ9 | VST3 coordinate: native normalised is the default (02 §1); does the probe show plugins with tapers bad enough to need a re-mapped coordinate? Yes for clap-wrapper builds: the test synth's 20 Hz-20 kHz cutoff is linear in its VST3 normalised value, and without a warp the VST3 fit stalled at 2-4 dB. The optimizer now warps any parameter whose probe response is log (04 §9). Open: how many third-party VST3 plugins need it | 🔬 |
| OQ11 | `estimate_f0` puts a pure sine at a subharmonic (220 Hz read as 31.4 Hz = f/7). Harmless for the probe (harmonic jumps are ignored) but it would mis-set `harm`/`env` for a sine target (WP2.2) | 🔬 |
| OQ10 | Resolved: baseline lowered to C++20 with an `expected` shim (see decision log); Clang 18 + libstdc++ 13 no longer blocked. Open detail: verify on MSVC and AppleClang once CI has run | 🔬 |

## Decision log

| Date | Decision |
| --- | --- |
| 2026-10-17 | Tool, not plugin; offline; processes for isolation; C++23 (not "C++22"), CMake >= 3.21; no frameworks; own CMA-ES port from Acidus |
| 2026-10-17 | **CLAP and VST3 are both first-class hosted formats** behind one `PluginHost` interface; any source format can be matched by any target format; VST3 hosting moved from "later" into WP1, scheduled right after the first working CLAP fit; the test synth is built as CLAP and VST3 (clap-wrapper) so both backends are tested against the same code; VST2, AU, LV2 out of scope for now |
| 2026-10-17 | `std::format` instead of `std::print` until the CI baseline has it (GCC 14); `std::expected` stays and sets the Clang baseline (OQ10) |
| 2026-10-17 | **C++20 instead of C++23** (owner: newer standard not important). `std::expected` is replaced by `mimicry::expected` (shim, aliases std when available); C++23 still builds. Supersedes the C++23 and `std::expected` decisions above |
| 2026-10-17 | License MIT (kept from the initial repo commit) |

## Progress log

- 2026-10-19: WP3 optimizer: `core/Cma` (CMA-ES, sep-CMA-ES, Jacobi), `core/Sampling` (LHS), `core/ParamSemantics`; library `mimicry_opt` with `Space`, `Objective`, `Embedding`, `Probe`, `Profile`, `Pipeline` (S1-S7, R restarts, top-K, categorical sweep and outer loop), `Fingerprint`, `Surrogate`. Commands `list`, `probe`, `fingerprint`, `fit` implemented. Found and fixed a WP1.5 bug: pool workers respawned on short-lived threads were killed by `PR_SET_PDEATHSIG` (regression check in `worker_*`). Two spec decisions from measurements: pitch-only parameters are pinned when S0 applies pitch (a detune basin is ~20 cents wide and flat elsewhere), and a probe-measured log response is warped in either format (OQ9). Tests: `opt` (T5 and units), `fit_clap`/`fit_vst3` (probe classes, safe box from the NaN and hang hazards, profile round trip, preset recovered < 1 dB, fingerprint neighbours). All 11 ctest tests pass on Linux/GCC 13 with the clap submodule and VST3 SDK.
- 2026-10-19: WP2 features and loss in `src/features` (library `mimicry_features`, test `mimicry_features_test`): pre-processing, f0, `msstft`/`env`/`bands`/`harm`/`inter`/`harmt`/`shape`, noise-floor clamp, closed-form gain, weights and kind classifier, onset offset, repeatability policy, `opt::RenderCache`. `Fft` inner loop rewritten without `std::complex` multiplication (about 3x faster). Open: pitch-bend calibration (2.2), seed pinning (2.5), feature cost still ~27 ms per 2 s candidate (OQ2). The CLAP/VST3 host tests are not built in this checkout (clap submodule absent), so only smoke, core and features ran.

- 2026-10-17: Specification written (this document set). No code yet.
- 2026-10-17: Specs updated for VST3 hosting: 01, 02 (rewritten around `PluginHost`), 05 §9, 06, 07 (T13-T16, `.vstpreset`), this plan.
- 2026-10-17: Standard lowered to C++20 with `src/core/Expected.hpp`; builds and passes smoke test on GCC 13 in both C++20 and C++23 modes; spec 01 §3, CLAUDE.md, README updated.
- 2026-10-17: Repository initialised: CMake skeleton (`mimicry_core`, `mimicry_host`, `mimicry`,
  `mimicry_worker`, CLAP test synth stub, smoke test), `clap` submodule, CI workflow for Linux/Windows/macOS,
  `CLAUDE.md`, and the spec copied into `docs/`. Verified locally with GCC 13 (0 warnings, smoke test passes);
  Windows/macOS CI not yet run. Spec 01 §3 compiler baseline corrected.
- 2026-10-17: WP0.3 core utilities (Rng, Json, Wav, Fft) with `mimicry_core_test`.
- 2026-10-17: WP1 host layer. `Engine` (per-format primitive) + `Renderer` (shared render core) + CLAP backend + VST3 backend
  on the SDK's hosting classes (SDK v3.8.0 fetched by `cmake/Vst3Sdk.cmake`, MIT). Test synth with planted hazards built as CLAP
  and, through clap-wrapper (pinned commit of holstebroe/clap-wrapper vst3-midi-out), as VST3. Worker process and supervisor
  (`WorkerClient`, `WorkerPool`): line-framed JSON on descriptors 3/4, anonymous shared memory on 5, watchdog, respawn, recycling.
  `ctest`: smoke, core, host_clap, host_vst3, equivalence_clap_vst3 (T13), worker_clap, worker_vst3 (T7/T9). Verified on Linux/GCC 13
  only; Windows isolation not implemented, Win32/macOS code paths unbuilt.
