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
| `spec/01_ARCHITECTURE.md` | Product shape, formats, constraints (CMake, C++23), processes, layout, commands, plugin profile |
| `spec/02_HOSTING.md` | `PluginHost` interface, CLAP and VST3 backends, concept mapping, threads, parameters, render loop, state, build |
| `spec/03_FEATURES_LOSS.md` | Features, weights, level, noise floor, cost |
| `spec/04_OPTIMIZER.md` | Encoding, semantics, probe, stage pipeline, CMA-ES, diversity, fingerprint, mapping |
| `spec/05_RESILIENCE.md` | Isolation, outcomes, search under failure, failure bisection, state poisoning, format-specific notes |
| `spec/06_TARGETS.md` | WAV, plugin-to-plugin (cross-format), image, effects |
| `spec/07_OUTPUT_VALIDATION.md` | Run output, `.vstpreset` export, generalisation checks, tests T1-T16, test synth |

## Build and run

```
git submodule update --init                       # clap
cmake -B build -DCMAKE_BUILD_TYPE=Release         # VST3 SDK fetched, or -DVST3_SDK_ROOT=... (not yet wired, WP1.7)
cmake --build build -j
ctest --test-dir build
build/mimicry list path/to/plugin.clap            # or plugin.vst3 (target; commands are stubs today)
build/mimicry fit --plugin path/to/plugin.vst3 --wav target.wav --minutes 20
```

## Work packages

### WP0 — Bootstrap
| ID | Issue | Status |
| --- | --- | --- |
| 0.1 | CMake project, C++23, `clap` submodule, VST3 SDK fetch / `VST3_SDK_ROOT`, `MIMICRY_VST3_HOSTING` option, IPO in Release, CI (Linux/Windows/macOS) | 🟡 skeleton builds on GCC 13; CI workflow written, Windows/macOS unverified until first push; SDK fetch comes with 1.7 |
| 0.2 | `CLAUDE.md`, evidence-tag conventions | ✅ |
| 0.3 | Core utilities: Json, Wav, Fft, Rng | ⬜ |

### WP1 — Host and worker (spec 02, 05 §1)
| ID | Issue | Status |
| --- | --- | --- |
| 1.1 | `PluginHost` interface, `ParamDesc`, `Job`, outcome codes, format selection by extension | 🟡 interface skeleton, `Outcome`, `format_from_path`; `open()` returns an error |
| 1.2 | CLAP backend: loader, host API, thread-check, instance lifecycle | ⬜ |
| 1.3 | Parameter enumeration with units from value text (both backends) | ⬜ |
| 1.4 | Shared render core: events, settle, notes, tail, output checks | ⬜ |
| 1.5 | Worker process, shared-memory audio, supervisor pool, watchdog | ⬜ (worker is a stub) |
| 1.6 | `mimicry_test_synth` (CLAP) and `mimicry_test_synth_vst3` (clap-wrapper) with planted hazards | 🟡 CLAP stub with empty factory only; clap-wrapper (holstebroe/clap-wrapper, branch vst3-midi-out) not yet added |
| 1.7 | **VST3 backend**: SDK hosting module loading, component + controller + connection, host classes, `ProcessData`, parameter changes, event list, offline setup | ⬜ |
| 1.8 | VST3 state (component + controller), program lists, program-change pinning, restart requests, tail/latency | ⬜ |
| 1.9 | Main-thread message pump in the worker (Win32 / macOS / Linux) | ⬜ |

### WP2 — Features and loss (spec 03)
| ID | Issue | Status |
| --- | --- | --- |
| 2.1 | `msstft`, `env`, `bands`, noise-floor clamp, closed-form gain | ⬜ |
| 2.2 | f0 estimation and pitch application (VST3 tuning cents / CLAP note expression / bend calibration) | ⬜ |
| 2.3 | `harm`, `inter`, `harmt` for pitched targets (port from Acidus) | ⬜ |
| 2.4 | `shape` features, weight presets by sample kind | ⬜ |
| 2.5 | Repeatability floor, render cache | ⬜ |

### WP3 — Optimizer (spec 04)
| ID | Issue | Status |
| --- | --- | --- |
| 3.1 | CMA-ES port from Acidus with restarts, Jacobi eigensolver; sep-CMA-ES | ⬜ |
| 3.2 | Unit-box encoding: CLAP plain values with log/linear decode, VST3 native normalised coordinate (02 §1), stepped handling, boundary penalty | ⬜ |
| 3.3 | Unsupervised pipeline (S1, S5, S7), time cap, patience | ⬜ |
| 3.4 | Semantics classifier and staged hybrid pipeline | ⬜ |
| 3.5 | Sensitivity probe, dead-parameter detection, pair probes, plugin profile (per format) | ⬜ |
| 3.6 | Categorical outer loop; R restarts; top-K diversity | ⬜ |
| 3.7 | Fingerprint database and nearest-neighbour seeding | ⬜ |
| 3.8 | Surrogate / Bayesian optimisation for slow plugins | ⬜ |

### WP4 — Resilience (spec 05)
| ID | Issue | Status |
| --- | --- | --- |
| 4.1 | Ranking of invalid candidates, resample, sigma adaptation | ⬜ |
| 4.2 | Safe box from probe | ⬜ |
| 4.3 | Failure bisection and constraint store | ⬜ |
| 4.4 | State reset, canary, instance recycling (both formats) | ⬜ |
| 4.5 | Failure reports and `mimicry replay` | ⬜ |
| 4.6 | Format-specific handling (05 §9): GUI-expecting VST3 plugins, restart requests, exceptions | ⬜ |

### WP5 — Targets (spec 06)
| ID | Issue | Status |
| --- | --- | --- |
| 5.1 | WAV target analysis and `mimicry fit` | ⬜ |
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
| 6.1 | `best.state`, `best.params.json`, `match.wav`, `ab.wav`, `report.md`, `diff.ppm` | ⬜ |
| 6.2 | Holdout and `mimicry eval` | ⬜ |
| 6.3 | Tests T1-T12 | 🟡 smoke test only (outcomes, severity, format detection) |
| 6.4 | `mimicry rehearse` round trip | ⬜ |
| 6.5 | `.vstpreset` writer and round-trip test (T14) | ⬜ |
| 6.6 | Backend equivalence, cross-format transfer, VST3 specifics (T13, T15, T16) | ⬜ |

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
| OQ2 | Performance target on the real machine (evals/s for Acidus, Tannhauser); measure before tuning weights | 🔬 |
| OQ3 | Default weights and probe plan are guesses; tune on real samples and on Acidus/Tannhauser presets | 🔬 |
| OQ4 | Resolved: `.vstpreset` export is in scope (WP5.8, WP6.5). Open detail: wrapper-state round trip across clap-wrapper versions | 🔬 |
| OQ5 | Plugins that need a message loop, window or licence dialog to initialise (mostly third-party VST3): how far does the minimal main-thread pump get us? Measure on real plugins (WP1.9, WP4.6) | 🔬 |
| OQ6 | Whether `ImageFeatures` is extracted into a shared header with the image-seeded synth | 🔬 |
| OQ7 | VST3 SDK: licence of the fetched version, and that its hosting sources build cleanly under C++23 on MSVC, GCC and Clang | 🔬 |
| OQ8 | VST3 offline processing mode vs realtime: how many real plugins sound different, and should the fit offer `--realtime-mode`? | 🔬 |
| OQ9 | VST3 coordinate: native normalised is the default (02 §1); does the probe show plugins with tapers bad enough to need a re-mapped coordinate? | 🔬 |
| OQ10 | Compiler baseline: `std::expected` rules out Clang < 19 with libstdc++ (found with Clang 18 + libstdc++ 13). Keep the baseline in 01 §3, or add a fallback `expected` shim? | 🔬 |

## Decision log

| Date | Decision |
| --- | --- |
| 2026-10-17 | Tool, not plugin; offline; processes for isolation; C++23 (not "C++22"), CMake >= 3.21; no frameworks; own CMA-ES port from Acidus |
| 2026-10-17 | **CLAP and VST3 are both first-class hosted formats** behind one `PluginHost` interface; any source format can be matched by any target format; VST3 hosting moved from "later" into WP1, scheduled right after the first working CLAP fit; the test synth is built as CLAP and VST3 (clap-wrapper) so both backends are tested against the same code; VST2, AU, LV2 out of scope for now |
| 2026-10-17 | `std::format` instead of `std::print` until the CI baseline has it (GCC 14); `std::expected` stays and sets the Clang baseline (OQ10) |
| 2026-10-17 | License MIT (kept from the initial repo commit) |

## Progress log

- 2026-10-17: Specification written (this document set). No code yet.
- 2026-10-17: Specs updated for VST3 hosting: 01, 02 (rewritten around `PluginHost`), 05 §9, 06, 07 (T13-T16, `.vstpreset`), this plan.
- 2026-10-17: Repository initialised: C++23 CMake skeleton (`mimicry_core`, `mimicry_host`, `mimicry`,
  `mimicry_worker`, CLAP test synth stub, smoke test), `clap` submodule, CI workflow for Linux/Windows/macOS,
  `CLAUDE.md`, and the spec copied into `docs/`. Verified locally with GCC 13 (0 warnings, smoke test passes);
  Windows/macOS CI not yet run. Spec 01 §3 compiler baseline corrected.
