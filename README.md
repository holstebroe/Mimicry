# Mimicry

Offline tool that hosts a CLAP or VST3 plugin and uses a non-linear optimizer to find the preset that
matches a WAV, an image, or another plugin's preset. C++20, CMake, no frameworks.

**Status: first working fit.** CLAP and VST3 hosting in isolated worker processes, the feature loss and
the optimizer (CMA-ES pipeline, sensitivity probe, fingerprints) are in place: `mimicry list`, `probe`,
`fingerprint`, `fit` and `harvest` (automatic preset harvest) work. Plugin-to-plugin transfer, image
targets, reports and `.vstpreset` export are next: see `docs/PROJECT_PLAN.md` for the work packages and
their order.

## What it will do

- `mimicry fit`: match a WAV with an instrument plugin (CLAP or VST3), safely and offline, and write a
  preset plus A/B audio.
- `mimicry transfer`: match a source plugin's presets (or a whole bank) in a target plugin, in any
  format combination, for instruments and effects (effects are probed with built-in test signals).
- `mimicry harvest` / `capture`: collect a plugin's presets as verified states, by whatever method it
  supports (preset files and folders, CLAP preset discovery, VST3 program lists, program change), or on
  Windows by picking them in the plugin's own editor, including a taught "next preset" click.
- `mimicry map`: map one plugin's parameters to another's and to roles such as cutoff or attack.
- `mimicry doctor`: a report of what a plugin and each of its parameters does (frequency response,
  distortion, dynamics, impulse response for effects; per-parameter curves for instruments).
- `mimicry image`: match an image, through audio descriptors or as a target spectrogram.
- Resilient by design: plugins are rendered in separate worker processes, so a plugin that crashes or
  hangs on an illegal parameter combination is scored as a failure and the search continues.

The optimizer (CMA-ES, staged fits, closed-form level and pitch) follows the approach used to fit
[Acidus](https://github.com/holstebroe/Acidus) to a hardware TB-303.

## Build

```
git clone --recurse-submodules https://github.com/holstebroe/Mimicry
cd Mimicry
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build
build/mimicry --help
```

Needs CMake 3.21 or newer and a C++20 compiler with `<format>`: MSVC 19.37 (VS 2022 17.7), GCC 13, or
Clang 17+ (AppleClang 15). C++23 also builds (`-DCMAKE_CXX_STANDARD=23`). If the clap submodule is missing, run `git submodule update --init`. The VST3 SDK is fetched at configure time (or
taken from `-DVST3_SDK_ROOT=...`) once the VST3 backend lands (WP1.7).

## Layout

```
src/core/        utilities: Json, Wav, Fft, Rng, Ipc, Cma (CMA-ES), Sampling, ParamSemantics
src/features/    pitch, features and loss
src/host/        PluginHost interface, shared render core; clap/ and vst3/ backends
src/worker/      mimicry_worker, the only process that loads plugins
src/supervisor/  worker client and pool with watchdog
src/targets/     preset harvest; WAV, plugin and image targets (planned)
src/opt/         encoding, objective, probe and profile, pipeline, fingerprints, surrogate
src/output/      failure reports, preset files (params.json, state); reports and .vstpreset planned
src/app/         the mimicry command line
src/tests/       tests and the test synth (CLAP, and VST3 through clap-wrapper)
docs/            PROJECT_PLAN.md and spec/
```

## Documentation

- [`docs/PROJECT_PLAN.md`](docs/PROJECT_PLAN.md): work packages, open questions, decision log.
- [`docs/spec/`](docs/spec/): architecture, hosting, features and loss, optimizer, resilience,
  targets, output and tests, diagnosis report.
- [`CLAUDE.md`](CLAUDE.md): development rules for humans and AI agents.

## License

MIT, see [LICENSE](LICENSE).
