# Mimicry

Offline tool that hosts a CLAP or VST3 plugin and uses a non-linear optimizer to find the preset that
matches a WAV, an image, or another plugin's preset. C++20, CMake, no frameworks.

**Status: skeleton.** The specification and plan are written (`docs/`), the build runs, and the
command line prints its interface. Hosting, rendering and optimisation are not implemented yet: see
`docs/PROJECT_PLAN.md` for the work packages and their order.

## What it will do

- `mimicry fit`: match a WAV with an instrument plugin (CLAP or VST3), safely and offline, and write a
  preset plus A/B audio.
- `mimicry transfer`: match a source plugin's presets (or a whole bank) in a target plugin, in any
  format combination.
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
src/core/        types and utilities (outcomes, later: Wav, Fft, Features, Cma, ...)
src/host/        PluginHost interface; clap/ and vst3/ backends (planned)
src/worker/      mimicry_worker, the only process that loads plugins
src/supervisor/  worker pool, scheduler, watchdog (planned)
src/targets/     WAV, plugin and image targets (planned)
src/opt/         optimizer pipeline, probes, fingerprint (planned)
src/output/      reports, A/B audio, preset export (planned)
src/app/         the mimicry command line
src/tests/       tests and the test synth (a stub for now)
docs/            PROJECT_PLAN.md and spec/
```

## Documentation

- [`docs/PROJECT_PLAN.md`](docs/PROJECT_PLAN.md): work packages, open questions, decision log.
- [`docs/spec/`](docs/spec/): architecture, hosting, features and loss, optimizer, resilience,
  targets, output and tests.
- [`CLAUDE.md`](CLAUDE.md): development rules for humans and AI agents.

## License

MIT, see [LICENSE](LICENSE).
