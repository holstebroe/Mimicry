# Development guidelines (humans and AI agents)

Mimicry hosts a CLAP or VST3 plugin offline and optimises its parameters to match a WAV, an image, or
another plugin's preset (in any format combination). C++20, CMake, no third-party frameworks (same
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

1. C++20 (`CMAKE_CXX_STANDARD 20`, CMake >= 3.21); C++23 also builds. Use `mimicry::expected`
   (`src/core/Expected.hpp`, std::expected when available) for fallible calls; no
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

## Scope and compatibility

- Assume every project is pre-release with no existing users, saved data, or external consumers, unless I say otherwise or the repo clearly shows otherwise (tagged releases, a migrations folder, a changelog, a versioned public API).
- Do not add compatibility shims, legacy fallbacks, deprecated aliases, data migrations, or version checks for old formats. When a design changes, replace the old behavior outright and delete the old code.
- If a change would break something that really matters (for example a file format I've already shipped), ask once before adding compatibility code.

## Minimal changes

- Default to the smallest change that solves the stated problem. Before writing code, ask: can this be fixed by changing a line, or by deleting something?
- Prefer deleting or simplifying existing code over adding new code.
- Accept small compromises to keep a diff small. An edge case behaving slightly differently is fine if you tell me.
- Don't add abstractions, helpers, config options, parameters, interfaces, or new files for a single use. Wait until there are at least three real call sites.
- Don't add defensive code for cases that can't happen (extra null checks, try/catch around internal calls, validation between internal functions). Validate only at system boundaries: user input, network, files.
- Don't refactor, rename, reformat, or clean up code unrelated to the task. If you notice something worth fixing, mention it in one line.
- Don't add tests, docs, or comments beyond what I asked for. Comments explain why, never what.
- If a change seems to need more than ~30 lines or touches more than a couple of files, stop and propose the approach first, including a smaller alternative.

## Reporting tradeoffs

- After a change, state in one or two sentences what behavior changed or was dropped by keeping it minimal (e.g. "no longer handles X; handling it would need ~40 more lines").
- Don't silently preserve old behavior by adding code. If keeping it is costly, say so and let me decide.

## Suggesting refactors

- Implement the minimal fix first. Then, if a refactor would clearly make the code simpler, smaller, or more coherent, propose it in a short note after the change. Don't apply it until I approve.
- Good triggers: the same logic now exists in three or more places; the fix was awkward because of a structural problem; code, parameters, or branches became dead or redundant; two near-identical functions or types could become one.
- Don't suggest refactors that only change style, move code without simplifying it, or add indirection.
- Describe each suggestion in one or two lines with a concrete effect (e.g. "removes ~60 lines, merges parseA and parseB"). If you can't state a concrete benefit, don't suggest it.
- Suggest at most one or two per task. If I decline, don't raise it again in the same session.
