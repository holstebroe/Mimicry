# 02 — Offline plugin hosting (CLAP and VST3)

Mimicry hosts **CLAP and VST3 plugins as equals**. Everything above the host layer (features, loss,
optimizer, targets, output) sees one format-agnostic interface and never includes a CLAP or VST3
header. Any source format can be matched by any target format, so a VST3 synth can be mimicked by a
CLAP synth and the reverse.

## 1. The `PluginHost` interface [D]

```
PluginHost::open(path, plugin_uid)        -> expected<Instance, HostError>     // .clap or .vst3, by extension
Instance::info()                          -> { format, uid, name, vendor, version hash, kind }
Instance::parameters()                    -> vector<ParamDesc>
Instance::value_text(id, coord)           -> string                            // for units and semantics
Instance::presets()                       -> vector<PresetRef>                 // factory programs / files
Instance::render(Job)                     -> expected<Audio, Outcome>          // 05 §2 outcome codes
Instance::save_state() / load_state(blob) -> expected<Blob / void>
Instance::tail_samples() / latency()      -> int
Instance::reset()                         -> expected<void>                    // 05 §5
```

`ParamDesc` = `{ id (uint32), name, module path, units, steps (0 = continuous), default, flags (hidden,
readonly, bypass, program-change, automatable), coord }`. Both formats use 32-bit parameter ids [S: CLAP
`param_id`, VST3 `ParamID`]. The interface is in `src/host/PluginHost.hpp`; backends implement the smaller `Engine` (`src/host/Engine.hpp`) and the shared render core `Renderer` implements `Instance` on top of it [A]. `Job.coords` has one entry per `parameters()` entry; the render core ignores read-only entries and pins bypass and program-change to their default [A].

**Mimicry coordinate** `u ∈ [0,1]` (04 §1):
- **VST3**: the plugin's own *normalised* value. The plugin's taper is kept, since designers usually
  already log-map frequencies and times [D].
- **CLAP**: the *plain* value mapped linear or log by 04 §1.
The probe (04 §3) checks how uniform each parameter's effect is in `u` either way, so a poor taper is
detected, not assumed. 04 §1's log/linear decode applies to CLAP plain values only.

Format is chosen by extension (`.clap`, `.vst3`); `--format` overrides. `plugin_uid` is the CLAP id
string or the VST3 class id (TUID) plus name.

## 2. Mapping of concepts

| Concept | CLAP backend | VST3 backend |
| --- | --- | --- |
| Load | `dlopen`/`LoadLibrary`, `clap_entry.init`, `get_factory(plugin-factory)` | SDK hosting `Module::create(path)`, `IPluginFactory` (class category "Audio Module Class") |
| Instance | `create_plugin(host, id)`, `init` | `createInstance` of `IComponent`, query `IAudioProcessor`; `IEditController` is the same object or created from `getControllerClassId`, then both are connected through `IConnectionPoint` |
| Host object | `clap_host_t` | `IHostApplication` (+ `IMessage`/`IAttributeList`), `IComponentHandler` for the controller |
| Setup | `activate(rate, min, max)` | `setupProcessing(ProcessSetup{ kOffline, kSample32, max block, rate })`, bus arrangement (stereo out), `setActive(true)` |
| Start | `start_processing` | `setProcessing(true)` |
| Parameters | `clap.params` (plain min/max, `value_to_text`) | `IEditController` params (normalised, `getParamStringByValue`, `ParameterInfo.units`), units via `IUnitInfo` |
| Apply a value | `CLAP_EVENT_PARAM_VALUE` at time 0 | `ProcessData::inputParameterChanges` (`IParamValueQueue`, sample offset 0) **and** `setParamNormalized` on the controller, called on the main thread before block 0 (`Engine::apply_controller_values`) [A] |
| Notes | `CLAP_EVENT_NOTE_ON/OFF`, velocity 0..1 | `Event::kNoteOnEvent/kNoteOffEvent` through `IEventList`, velocity 0..1, `noteId` |
| Fine tuning | note-expression TUNING | `NoteOnEvent.tuning` (cents): native |
| Transport | `clap_event_transport` | `ProcessContext` (`kPlaying | kTempoValid | kTimeSigValid`, 120 BPM) |
| State | `clap.state` save/load | `IComponent::getState/setState`, `IEditController::setComponentState`, controller `getState/setState` |
| Tail / latency | `clap.tail`, `clap.latency` | `getTailSamples`, `getLatencySamples` |
| Reset | `deactivate` + `activate` (or `reset`) | `setProcessing(false)`, `setActive(false)`, `setActive(true)`, `setProcessing(true)` |
| Factory presets | preset-load / discovery extensions | `IUnitInfo` program lists (select through the program-change parameter) and `.vstpreset` files |
| Kind | note input + audio output = instrument; audio in+out = effect | event input bus + audio output = instrument; audio in+out = effect; class subcategories as a hint |

Rules for the VST3 backend:
- `IComponentHandler` callbacks (`beginEdit/performEdit/endEdit`) triggered by `setParamNormalized` are
  accepted and ignored: they must not recurse into the render path.
- `restartComponent` flags are honoured: a reload-component or param-titles-changed request makes the
  worker re-enumerate parameters and re-run setup, and the supervisor is told the parameter table changed.
- Plain 32-bit float processing is the default; 64-bit only if the plugin asks. Mono-only or unusual bus
  layouts are negotiated, and a plugin that refuses stereo out is rendered in its own layout and
  mixed to mono.
- A stepped parameter named Program / Preset / Patch is treated as a program-change parameter even without the flag (CLAP has none; wrappers do not always set VST3's) [I].
- Hide-flagged, read-only, and bypass parameters are excluded as in §4; the program-change parameter is
  pinned to a fixed program so a preset change cannot overwrite the candidate's parameters.

## 3. Threads [D]

Each worker has a main thread and one render thread created once. The host reports threads
truthfully to the plugin (`clap.thread-check` for CLAP; for VST3 the SDK's documented thread rules).
Create, init, setup, state, parameter reads and destroy run on the main thread; `process` and the
start/stop-processing calls on the render thread.

Many plugins (JUCE-based ones in particular, in either format) expect a **running message loop on the
main thread** for timers and async updates. The worker's main thread therefore pumps a minimal loop
between jobs (Win32 message pump, macOS run loop, a poll loop on Linux). No window is created. A plugin
that blocks waiting for a dialog or window is caught by the watchdog (05 §1).

## 4. Parameters

At load `mimicry list` records, per parameter: id, name, module path (CLAP module / VST3 unit chain),
min, max, default, flags, **units**, and the text for 0, 25, 50, 75, 100 % from the plugin's own
`value_to_text` / `getParamStringByValue`. The text gives units ("Hz", "ms", "s", "dB", "%", note names),
the best evidence for semantics and scaling (04 §1-§2).

Rules:
- Skip read-only and (by default) hidden parameters; skip bypass and pin it to "not bypassed".
- Never touch tempo-sync or transport parameters without being asked [D]; transport is a fixed 120 BPM.
- Ids are used verbatim and never renumbered; profiles key on format, plugin uid, id and name together,
  so a plugin update that reorders them is detected (version hash mismatch → re-probe).
- Plugin version hash = hash of the binary (file size + content hash), since neither format guarantees
  a trustworthy version string.

## 5. Render loop

A **render job** = full parameter vector + note plan + sample rate + block size.

```
reset instance (clean state, 05 §5)
block 0:  apply every active parameter at time 0                       (full vector, never a delta)
settle:   process silent blocks for settle_ms                          (smoothers converge; Acidus sends CCs 1.2 s early [S])
notes:    note-on / note-off at sample-exact times from the plan; velocity as given
tail:     keep processing until output is below -80 dBFS for 200 ms, or tail_max (uses the plugin's tail report if any)
```

Defaults [D]: sample rate 48 kHz (or the target's), block 512 frames, `settle_ms` 250 (`--settle-ms`;
1200 for plugins that smooth slowly), `tail_max` 8 s. VST3 is set up in **offline processing mode**,
which some plugins use to switch to higher-quality paths: this is deliberate and reported in the run
report (the sound can differ slightly from realtime mode).

Pitch: a note carries a key plus an optional **tuning offset in cents**: VST3 `tuning` natively, CLAP
note-expression TUNING when supported, else MIDI pitch bend with a measured range (a calibration render
at bend 0 and max gives the real bend range). If none exists, the nearest key is used and the residual
(up to 50 cents) is reported [I]. Pitch is *measured* from the target and applied directly, not
searched [S: Acidus measures tuning explicitly].

## 6. Output checks (in the worker)

All samples finite; peak below `peak_limit` (default **50** [S: Acidus rejects renders above 50]); not
silent (above -90 dBFS unless the target is silent); no runaway DC (|mean| below 0.5 of peak over the
last second). A failed check returns an outcome code, not audio. Exceptions thrown by plugin code
across a call are caught at the worker boundary and returned as `HostError`; faults that kill the
process are the supervisor's job (05 §1).

## 7. State and presets

`save_state` after the final parameters gives the plugin's opaque state (CLAP state blob; VST3
component state plus controller state), stored as `best.state`. `load_state` restores a known-good
snapshot before runs and after failures. Plugins without state support can only be restored via
parameters; the profile flags them. State blobs are format-specific and not portable between formats:
what *is* portable is the parameter-level match, which is what the optimizer produces.

Preset sources for `transfer`: CLAP preset-load/discovery, VST3 program lists and `.vstpreset`
files, plain state files, or a folder of any of these.

## 8. Build

- CLAP headers: the `clap` submodule [S: Acidus, Tannhauser].
- VST3: the SDK is fetched at configure time unless `VST3_SDK_ROOT` points at a local copy, as in
  Acidus's clap-wrapper setup [S: Acidus `CMakeLists.txt`]. CMake option `MIMICRY_VST3_HOSTING`
  (default ON). Compiled in: the SDK's hosting sources (module loading, plug-provider, host classes,
  parameter changes, event list, process data) plus base and pluginterfaces, with the per-platform module
  loader (Win32, macOS bundle, Linux). Check the SDK licence of the fetched version (plan, OQ7).
- Both backends link only into `mimicry_host` and `mimicry_worker`; the supervisor and the rest of the
  tool see the interface alone.

## 9. Not in scope [D]

VST2 (deprecated, SDK no longer distributed), AU and LV2. The `PluginHost` interface does not preclude
adding them later; AU and LV2 are a backend each.
