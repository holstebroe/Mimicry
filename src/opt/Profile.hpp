#pragma once
// Plugin profile (docs/spec/01_ARCHITECTURE.md section 7, 04_OPTIMIZER.md section 3): what the probe
// measured about a plugin, per format. Keyed on format + plugin uid + version hash; parameter entries are
// matched on id AND name (CLAUDE.md rule 7), so a renamed or renumbered parameter is never mis-applied.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/Expected.hpp"
#include "core/Json.hpp"
#include "core/Outcome.hpp"
#include "core/ParamSemantics.hpp"
#include "host/PluginHost.hpp"
#include "opt/Constraints.hpp"

namespace mimicry::opt {

// What a parameter measurably does (spec 04 section 3).
enum class ProbeClass { Dead, Pitch, Brightness, Time, Level, Spectrum, Fx };
[[nodiscard]] std::string_view to_string(ProbeClass c) noexcept;
[[nodiscard]] std::optional<ProbeClass> parse_probe_class(std::string_view text) noexcept;
// Group a measured class implies, Unknown for Dead and Spectrum (the name guess stays).
[[nodiscard]] ParamGroup group_of(ProbeClass c) noexcept;

// How the dominant descriptor changes along u: "linear", "log" (most of the change in the first quarter,
// so the effect is uniform in log) or "exp" (most in the last quarter).
enum class Response { Linear, Log, Exp };

struct ParamProfile {
    std::uint32_t id = 0;
    std::string name;
    Semantics names;                 // the name guess
    ProbeClass measured = ProbeClass::Spectrum;
    double effect = 0;               // largest embedding change over the floor-based threshold
    Response response = Response::Linear;
    double safe_lo = 0, safe_hi = 1; // safe box in u (spec 05 section 3)
    std::vector<Outcome> outcomes;   // at the probe values 0, .25, .5, .75, 1
    [[nodiscard]] bool dead() const noexcept { return measured == ProbeClass::Dead; }
};

struct PairProfile {
    std::uint32_t a = 0, b = 0;
    double interaction = 0;  // RMS of the 2x2 interaction vector (embedding units, dB-like)
    bool separable = true;
};

struct Profile {
    host::PluginFormat format = host::PluginFormat::Clap;
    std::string uid, name, vendor, version_hash;
    double floor_db = 0;          // repeatability floor (spec 03 section 6)
    double embedding_floor = 0;   // embedding distance between two renders of the defaults
    double render_ms = 0;         // mean wall time per render during the probe
    std::vector<ParamProfile> params;
    std::vector<PairProfile> pairs;
    std::vector<Constraint> constraints;  // learned by failure bisection (spec 05 section 4), kept by `probe`

    // False for a profile that only carries constraints (written by a fit before any probe).
    [[nodiscard]] bool probed() const noexcept { return !params.empty(); }

    // Entry for this parameter, only when both id and name match.
    [[nodiscard]] const ParamProfile* find(const host::ParamDesc& p) const;
    [[nodiscard]] bool matches(const host::PluginInfo& info) const;
    // False when the pair was probed and found to interact.
    [[nodiscard]] bool separable(std::uint32_t a, std::uint32_t b) const;

    [[nodiscard]] json::Value to_json() const;
    [[nodiscard]] static expected<Profile, std::string> from_json(const json::Value& v);
    [[nodiscard]] expected<void, std::string> save(const std::filesystem::path& path) const;
    [[nodiscard]] static expected<Profile, std::string> load(const std::filesystem::path& path);

    // profiles/<format>-<uid>-<version hash>.json under `dir` (plugin_file_path).
    [[nodiscard]] static std::filesystem::path default_path(const std::filesystem::path& dir, const host::PluginInfo& info);
};

// <dir>/<format>-<uid>-<version hash><extension>, unsafe file name characters as '_': the one naming rule
// of every per-plugin file (profile, fingerprint database).
[[nodiscard]] std::filesystem::path plugin_file_path(const std::filesystem::path& dir, const host::PluginInfo& info,
                                                     std::string_view extension);

}  // namespace mimicry::opt
