#pragma once
// Format-agnostic plugin host interface (docs/spec/02_HOSTING.md section 1).
//
// CLAP and VST3 are peers behind this interface. Nothing outside src/host/clap and src/host/vst3 may
// include a CLAP or VST3 header (CLAUDE.md rule 3). This is the first skeleton of the interface: the
// backends (WP1.2 CLAP, WP1.7 VST3) fill it in.

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
    int steps = 0;             // 0 = continuous
    double default_coord = 0;  // in the Mimicry coordinate (spec 02 section 1)
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
struct Job {
    std::vector<double> coords;  // one per active parameter, in ParamDesc order
    std::vector<Note> notes;
    int sample_rate = 48000;
    int block_size = 512;
    double settle_ms = 250;
    double tail_max_ms = 8000;
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

    [[nodiscard]] virtual expected<Audio, Outcome> render(const Job& job) = 0;

    [[nodiscard]] virtual expected<std::vector<std::uint8_t>, HostError> save_state() = 0;
    [[nodiscard]] virtual expected<void, HostError> load_state(const std::vector<std::uint8_t>& blob) = 0;
    [[nodiscard]] virtual expected<void, HostError> reset() = 0;
};

// Opens a plugin by path (format from the extension). `uid` picks one plugin of a multi-plugin file.
// Until the backends exist this always returns an error.
[[nodiscard]] expected<std::unique_ptr<Instance>, HostError> open(const std::filesystem::path& path,
                                                                        std::string_view uid = {});

}  // namespace mimicry::host
