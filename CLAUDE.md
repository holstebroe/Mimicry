# Development guidelines (humans and AI agents)

Mimicry hosts a CLAP or VST3 plugin offline and optimises its parameters to match a WAV, an image, or
another plugin's preset (in any format combination). C++23, CMake, no third-party frameworks (same
spirit as Acidus/Tannhauser). The VST3 SDK is the one required third-party dependency.

## Where things are

- Start with `docs/PROJECT_PLAN.md`: work packages, open questions, decision log. Keep it current:
  update the status of every issue you touch and add a line to the progress log.
- `docs/spec/` is the specification. Code and spec must agree; when you change a rule or a default,
  change the spec in the same commit, with its evidence tag ([S] sourced, [I] inferred, [D] default,
  [A] added).
- Optimizer ideas and constants come from Acidus (`tools/calibrate_reference.py`,
  `tools/fit_stage.py`, `docs/EMULATION_TRAINING_PLAN.md`); cite them as [S] when ported.

## Rules

1. C++23 (`CMAKE_CXX_STANDARD 23`, CMake >= 3.21). Use `std::expected` for fallible calls; no
   exceptions across the host/plugin boundary and none on the render path.
2. No JUCE or other frameworks. A new dependency needs an entry in the plan's open questions first.
3. CLAP and VST3 are peers. Format-specific code lives only in `src/host/clap/` and `src/host/vst3/`
   behind `PluginHost`; the optimizer, features, targets and output code never include a CLAP or VST3
   header. A feature that works for one format must work for the other (or the plan says why not).
4. Never render a plugin in the supervisor process; only `mimicry_worker` loads plugins
   (`--no-isolation` exists for debugging).
5. Every candidate renders from the **full** parameter vector after a state reset. Never send deltas.
6. A failed render is information: record the outcome, never discard it silently (spec 05).
7. Parameter ids are never renumbered or reinterpreted; profiles key on format + plugin uid + id +
   name + version hash.
8. Mimicry never modifies plugin files, user presets, or saved plugin state.
9. After host or worker changes run `ctest`, in particular T1, T7, T8, T9 and, for backend work, T13
   (spec 07 §4).
10. Features are phase-insensitive unless the spec says otherwise; add a T4 invariance case with any
    new feature.

## Build

```
git submodule update --init
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
ctest --test-dir build
```
