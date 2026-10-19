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
#include <memory>
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
};

// Opens a plugin by path (format from the extension) in THIS process. `uid` picks one plugin of a
// multi-plugin file. Only the worker (and debugging tools) call this; the supervisor goes through
// src/supervisor, which runs it in a worker process (CLAUDE.md rule 4).
[[nodiscard]] expected<std::unique_ptr<Instance>, HostError> open(const std::filesystem::path& path,
                                                                        std::string_view uid = {});

}  // namespace mimicry::host
