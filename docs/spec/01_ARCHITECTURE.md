# 01 — Architecture

Mimicry hosts a plugin (**CLAP or VST3**) offline, renders it, and searches its parameter space for
the preset whose sound is closest to a **target**: a WAV, an image, or another plugin's preset. It
generalises what Acidus already does to fit itself to a hardware TB-303 (`tools/calibrate_reference.py`,
`tools/fit_stage.py`, `docs/EMULATION_TRAINING_PLAN.md`).

Evidence tags as in Tannhauser: **[S]** sourced (from Acidus/Tannhauser code or docs), **[I]**
inferred, **[D]** default chosen by this project (tune later), **[A]** added/later feature.

## 1. Product shape

Mimicry is a **tool**, not a plugin: an offline command-line program first, a small
software-rendered GUI later. Hosting a plugin inside a plugin adds thread, window and crash problems
for no gain [D]. A later companion plugin, **Mimicry Capture** (CLAP + VST3 via clap-wrapper, as the
other plugins), could record a take in the DAW and hand it to the tool as a target [A].

## 2. Plugin formats

CLAP and VST3 are **both first-class** hosted formats, behind one `PluginHost` interface (02 §1).
The optimizer, features, targets and output code never include a CLAP or VST3 header, and any source
format can be matched by any target format. Out of scope: VST2, AU, LV2 (a backend each if ever added;
02 §9).

## 3. Constraints (project rules)

- CMake, **C++20** [D: the owner does not need C++23 and an older standard is easier to build on;
  "C++22" does not exist]. `CMAKE_CXX_STANDARD` defaults to 20 and `-DCMAKE_CXX_STANDARD=23` also
  builds (both checked on GCC 13.3). CMake >= 3.21. Compiler baseline [I]: MSVC 19.37 (VS 2022 17.7),
  GCC 13, Clang 17 or newer (libc++ or libstdc++ 13), AppleClang 15 — driven by `std::format`.
  `std::expected` is C++23, so `mimicry::expected` (`src/core/Expected.hpp`) is a small shim with
  the same call-site API that aliases `std::expected` when the library has it. `std::print` is not
  used.
- No JUCE or other frameworks. Small leaf dependencies need an explicit decision (see plan,
  open questions): the plan is own WAV, FFT, JSON and PPM code. The **VST3 SDK** is the one required
  third-party code, as the format's own interface definition and hosting classes; it is fetched at
  configure time or taken from `VST3_SDK_ROOT`, as Acidus does for clap-wrapper [S].
- CLAP headers via the `clap` submodule, as in Acidus/Tannhauser [S].
- The existing repos stay on C++17 [S: both `CMakeLists.txt`]; Mimicry shares no compiled code with
  them. Anything copied back (e.g. feature extractors) must stay C++17-compatible. The VST3 SDK sources
  must build under C++20 in CI (plan, OQ7).
- Language features to use: `mimicry::expected` for fallible calls, `std::span`, `std::jthread` /
  `std::stop_token`, `std::format`, ranges (all C++20). No exceptions
  across the host/plugin boundary and none on the render path (plugin-thrown exceptions are caught at
  the worker boundary).

## 4. Processes

```
 mimicry (supervisor / CLI)                      mimicry_worker  (one per core)
 ┌───────────────────────────────┐   pipe/socket ┌───────────────────────────────────┐
 │ target analysis               │──────────────►│ load .clap or .vst3, one instance  │
 │ optimizer (CMA-ES, stages)    │   job: params │ apply params, settle, play notes   │
 │ candidate queue + cache       │               │ render offline, faster than real   │
 │ watchdog (timeouts, respawn)  │◄──────────────│ checks: finite, peak, silence      │
 │ constraint store              │ shared memory │ audio -> shared-memory ring        │
 └───────────────────────────────┘               └───────────────────────────────────┘
```

Why processes: a plugin that dereferences null on an illegal parameter combination takes its whole
process down, and that cannot be caught in-process (SIGSEGV, SEH). The supervisor survives, scores the
candidate as a crash, and respawns the worker (see 05_RESILIENCE). The same holds for both formats.

Features are computed in the **supervisor** from the returned audio [D]: workers stay thin, and the
feature code is one place.

## 5. Source layout (mirrors Acidus/Tannhauser)

```
src/core/        Types, Rng, Json (minimal), Wav, Fft, Features, Loss, Cma (cma + sep-cma),
                 Sampling (Sobol/LHS), ParamSemantics, Constraints
src/host/        PluginHost.hpp (interface), Renderer (job -> blocks, shared by backends),
                 clap/ (loader, HostApi, instance), vst3/ (module, component/controller,
                 host classes, ProcessData, state, .vstpreset), MessagePump
src/worker/      worker_main, Ipc, SharedMem
src/supervisor/  Pool, Scheduler, Watchdog, RenderCache
src/targets/     WavTarget, PluginTarget, ImageTarget, ProbePlan
src/opt/         Pipeline (stages), Probe (sensitivity), Diversity, Fingerprint
src/output/      Report, AbWav, Ppm, PresetExport (state, params.json, .vstpreset)
src/app/         main + commands
src/gui/         (later) Graphics/Font/ModernDraw/window glue taken from Acidus
src/tests/       unit + integration executables; test_synth/ (hazard plugin, built as CLAP and VST3)
tools/           analysis helpers
docs/            plan, spec/
```

CMake targets: `mimicry_core`, `mimicry_host` (static libs), `mimicry_worker`, `mimicry` (executables),
`mimicry_test_synth` (CLAP module with planted hazards) and `mimicry_test_synth_vst3` (the same code
wrapped by clap-wrapper, as the other plugins), test executables registered with `ctest`. Release
builds use IPO and size-optimise glue as in Acidus [S].

## 6. Commands

`<plugin>` is a `.clap` or `.vst3` path in every command.

| Command | What |
| --- | --- |
| `mimicry list <plugin>` | Enumerate parameters with semantics guess, units, flags |
| `mimicry probe <plugin>` | Sensitivity + safe-range + hazard probe; writes the plugin profile |
| `mimicry fit --plugin P --wav T.wav` | Match a WAV |
| `mimicry transfer --source A --target B` | Match source presets (one, several, or all) in B; A and B may be different formats |
| `mimicry image --plugin P --image I.png` | Match an image (descriptor or spectrogram route) |
| `mimicry fingerprint <plugin>` | Build the random-preset feature database |
| `mimicry rehearse <plugin>` | Round-trip self-test (plugin as its own target) |
| `mimicry eval --plugin P --state S --wav T.wav` | Score without fitting [S: `--evaluate-only`] |
| `mimicry replay <crash.json>` | Re-run a recorded failure, in-process, for debugging |

## 7. Plugin profile

`profiles/<format>-<plugin-uid>-<version-hash>.json`, written by `probe` and updated by every run (like
Acidus's `calibrations/*.json` [S]): parameter semantics and confidence, log/linear choice, dead
parameters, safe box per parameter, learned hazard constraints, repeatability floor, render cost.
The same plugin hosted as CLAP and as VST3 has two profiles (parameter ids, tapers and the offline
mode can differ). A profile makes every later run on the same plugin faster and safer.
