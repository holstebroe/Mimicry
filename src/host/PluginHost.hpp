#pragma once
// Format-agnostic plugin host interface (docs/spec/02_HOSTING.md section 1).
//
// CLAP and VST3 are peers behind this interface. Nothing outside src/host/clap and src/host/vst3 may
// include a CLAP or VST3 header (CLAUDE.md rule 3). The backends implement the smaller `Engine`
// interface (host/Engine.hpp); the shared render core (host/Renderer.hpp) turns an Engine into an
// Instance, so the render loop, settle, tail and output checks are written once for both formats.

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/Expected.hpp"
#include "core/Outcome.hpp"

namespace mimicry::host {

enum class PluginFormat { Clap, Vst3 };

[[nodiscard]] std::string_view to_string(PluginFormat format) noexcept;

// Format from the file name: ".clap" or ".vst3" (case-insensitive; a trailing separator is fine, since
// a .vst3 is a bundle directory on Linux and macOS).
[[nodiscard]] expected<PluginFormat, std::string> format_from_path(const std::filesystem::path& path);

enum class PluginKind { Instrument, Effect, Unknown };

struct ParamFlags {
    bool hidden = false;
    bool readonly = false;
    bool bypass = false;
    bool program_change = false;  // pinned, never searched (spec 05 section 9)
    bool automatable = true;
};

struct ParamDesc {
    std::uint32_t id = 0;      // verbatim plugin id, never renumbered
    std::string name;
    std::string module;        // CLAP module path / VST3 unit chain
    std::string units;         // from the plugin's own value text
    int steps = 0;             // 0 = continuous, else the number of distinct values
    double min = 0;            // plain range (CLAP) or 0..1 (VST3 normalised)
    double max = 1;
    double default_plain = 0;
    bool log_scale = false;    // decode u logarithmically (CLAP only, spec 04 section 1)
    double default_coord = 0;  // in the Mimicry coordinate u in [0,1] (spec 02 section 1)
    std::array<std::string, 5> texts;  // the plugin's own value text at 0, 25, 50, 75, 100 %
    ParamFlags flags;
};

struct PluginInfo {
    PluginFormat format = PluginFormat::Clap;
    std::string uid;           // CLAP id string / VST3 class id
    std::string name;
    std::string vendor;
    std::string version_hash;  // hash of the plugin binary
    PluginKind kind = PluginKind::Unknown;
};

struct HostError {
    std::string message;
};

// One note of a note plan. Key plus an optional tuning offset in cents.
struct Note {
    int key = 60;
    double cents = 0;
    float velocity = 100.0f / 127.0f;
    double on_ms = 0;
    double off_ms = 0;
};

// A render job: the FULL parameter vector, never a delta (CLAUDE.md rule 5).
//
// `coords` has one entry per parameter of `Instance::parameters()`, in that order. The render core
// ignores the entries of read-only parameters and forces bypass and program-change parameters to
// their default, so a candidate can never mute the plugin or overwrite its own parameters.
struct Job {
    std::vector<double> coords;
    std::vector<Note> notes;
    int sample_rate = 48000;
    int block_size = 512;
    double settle_ms = 250;
    double tail_max_ms = 8000;
    double peak_limit = 50;      // spec 05 section 2 [S: Acidus]
    bool allow_silent = false;   // the target itself is silent
    // > 0: a note's cents go out as MIDI pitch bend with this measured range in semitones instead of the
    // format's tuning (spec 02 section 5, for plugins that ignore tuning). Bend is channel-wide.
    double bend_range = 0;
    // Non-empty: a source render of a harvested preset (spec 02 section 7). After the reset the render core
    // loads this complete state instead of sending `coords` (which may then be empty). Still a full state,
    // never a delta (CLAUDE.md rule 5).
    std::vector<std::uint8_t> state;
};

// How a preset is applied (docs/spec/02_HOSTING.md section 7), in the order the harvest ladder tries them
// (06 section 6).
enum class PresetMethod {
    File,          // a state file: CLAP state blob, .vstpreset, or (opt-in) a raw plugin file as state
    ClapPreset,    // CLAP preset discovery + preset-load
    ProgramData,   // VST3 IProgramListData::getProgramData
    ProgramList,   // VST3 IUnitInfo program list through its program-change parameter
    MidiProgram,   // MIDI program change (CLAP, note port with the MIDI dialect)
    ProgramParam,  // heuristic: a stepped parameter named Program / Preset / Patch, or a bare program-change parameter
    Capture,       // picked by the user (or a taught click) in the plugin's own editor (06 section 6)
};
[[nodiscard]] std::string_view to_string(PresetMethod method) noexcept;
[[nodiscard]] std::optional<PresetMethod> parse_preset_method(std::string_view text) noexcept;

// The plugin's own editor (Windows only, docs/spec/02_HOSTING.md section 3): hosted in a window of the worker, for
// manual capture (06 section 6). `parent` is the HWND the plugin attaches to.
struct EditorSize {
    int width = 0, height = 0;
    bool resizable = false;
};
struct EditorEvents {
    std::function<void(int width, int height)> resize;  // the plugin asks for this client size
    std::function<void()> closed;                       // the plugin closed its own view
};

// One preset a plugin offers or a file holds. Which fields count depends on the method.
struct PresetRef {
    PresetMethod method = PresetMethod::File;
    std::string name;              // from the plugin where the format offers one, else empty
    std::string location;          // File: the path. ClapPreset: the file location, "" for the plugin's own
    std::string load_key;          // ClapPreset: the load key, "" for none
    bool location_is_file = false; // ClapPreset: CLAP_PRESET_DISCOVERY_LOCATION_FILE
    std::uint32_t list_id = 0;     // ProgramData / ProgramList: the VST3 program list id
    std::uint32_t param_id = 0;    // ProgramList / ProgramParam: the program-change parameter
    int index = 0;                 // program index (MidiProgram: the program number)
    int count = 0;                 // programs in the list, or values of the parameter
    int bank = -1;                 // MidiProgram: bank select (CC 0 / CC 32), -1 for none
};

// A loaded preset, normalised (spec 02 section 7): the plugin's complete state plus every parameter read
// back in the Mimicry coordinate, so a source can render from either.
struct CapturedPreset {
    std::vector<std::uint8_t> state;
    std::vector<double> coords;    // one per parameters() entry
    std::string name;
};

struct Audio {
    int sample_rate = 0;
    int channels = 0;
    std::vector<float> samples;  // interleaved
};

class Instance {
public:
    virtual ~Instance() = default;

    [[nodiscard]] virtual const PluginInfo& info() const = 0;
    [[nodiscard]] virtual const std::vector<ParamDesc>& parameters() const = 0;
    [[nodiscard]] virtual std::string value_text(std::uint32_t id, double coord) const = 0;

    // Audio starts at the first note (the settle period is not returned). On a failed output check the
    // outcome is returned and `last_message()` says why.
    [[nodiscard]] virtual expected<Audio, Outcome> render(const Job& job) = 0;
    [[nodiscard]] virtual const std::string& last_message() const = 0;
    [[nodiscard]] virtual int tail_samples() const = 0;
    [[nodiscard]] virtual int latency_samples() const = 0;
    // Bumped when the plugin changed its parameter table (VST3 restartComponent, CLAP rescan).
    [[nodiscard]] virtual std::uint64_t parameters_version() const = 0;
    // Main thread: lets the plugin run its pending main-thread callbacks and drains the system queue.
    virtual void pump() = 0;
    // Optional progress word, incremented (atomically) as every block of a render starts, so a supervisor
    // that killed a hung worker can tell "never reached the first block" (spec 05 section 9).
    // Null turns it off; the word must stay valid while set.
    virtual void set_block_counter(std::uint64_t* /*counter*/) {}

    [[nodiscard]] virtual expected<std::vector<std::uint8_t>, HostError> save_state() = 0;
    [[nodiscard]] virtual expected<void, HostError> load_state(const std::vector<std::uint8_t>& blob) = 0;
    [[nodiscard]] virtual expected<void, HostError> reset() = 0;

    // Presets the plugin itself declares: CLAP preset discovery, VST3 program lists and program data, MIDI
    // program change (CLAP), stepped program parameters (spec 02 section 7). Files are the caller's.
    [[nodiscard]] virtual expected<std::vector<PresetRef>, HostError> presets() = 0;
    // Loads one preset from a clean state, waits up to `wait_ms` for the plugin to finish (CLAP `loaded`,
    // a program change processed), and returns the captured state and parameters. The instance is back at
    // its clean state afterwards, so later renders are unaffected. Only harvest-type jobs call this (05
    // section 9), never a fit.
    [[nodiscard]] virtual expected<CapturedPreset, HostError> apply_preset(const PresetRef& ref, double wait_ms = 200) = 0;

    // Editor and manual capture (Windows, main thread; other platforms and the supervisor's WorkerClient answer
    // with an error). The editor attaches to `parent_window` (an HWND); `editor_resized` tells the plugin the
    // window was resized by the user.
    [[nodiscard]] virtual expected<EditorSize, HostError> open_editor(void* /*parent_window*/, EditorEvents /*events*/) {
        return unexpected(HostError{"plugin editors are hosted on Windows only"});
    }
    virtual void editor_resized(int /*width*/, int /*height*/) {}
    virtual void close_editor() {}
    // The plugin's state and every parameter as they are right now, after the user's edits in the editor: no
    // reset, no reload, the editor session goes on. `name` is kept in the result.
    [[nodiscard]] virtual expected<CapturedPreset, HostError> capture_current(const std::string& /*name*/) {
        return unexpected(HostError{"capture needs an in-process instance with an editor"});
    }
    // How many preset-change signals the plugin gave since the last call (CLAP preset-load `loaded`, a VST3
    // restart with changed values or edits): for watch mode (06 section 6).
    [[nodiscard]] virtual int take_change_signals() { return 0; }
    // The plugin's own editor in a top-level window, so the user can sign in or dismiss a dialog before a run
    // (spec 02 section 3). Windows only; elsewhere an error. The window lives on the main thread and is
    // serviced by `pump()` (the worker pumps while it waits for commands). Closing the window, or
    // `close_editor()` (also the end of a capture session), ends it; renders do not need it closed.
    [[nodiscard]] virtual expected<void, HostError> open_editor() {
        return unexpected(HostError{"the plugin editor can only be shown on Windows"});
    }
    // True while the editor window is open.
    [[nodiscard]] virtual bool editor_open() const { return false; }
};

// Opens a plugin by path (format from the extension) in THIS process. `uid` picks one plugin of a
// multi-plugin file. Only the worker (and debugging tools) call this; the supervisor goes through
// src/supervisor, which runs it in a worker process (CLAUDE.md rule 4).
[[nodiscard]] expected<std::unique_ptr<Instance>, HostError> open(const std::filesystem::path& path,
                                                                        std::string_view uid = {});

}  // namespace mimicry::host
