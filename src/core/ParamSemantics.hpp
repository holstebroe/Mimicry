#pragma once
// Parameter semantics from name, module path and units (docs/spec/04_OPTIMIZER.md section 2). Names are
// priors only: a measured probe signature overrides them (opt/Probe.hpp). Format-independent: it sees
// strings, never a plugin.

#include <string>
#include <string_view>

namespace mimicry {

enum class ParamGroup { Osc, Filter, Env, Lfo, Fx, Amp, Global, Unknown };
inline constexpr int kParamGroupCount = 8;

[[nodiscard]] std::string_view to_string(ParamGroup group) noexcept;
[[nodiscard]] ParamGroup parse_group(std::string_view text) noexcept;  // Unknown if not a group name

struct Semantics {
    ParamGroup group = ParamGroup::Unknown;
    double confidence = 0;  // 0..1
};

// Token match on the lower-cased name (weight 1) and module path (weight 0.6); units add a hint (Hz on
// a parameter that names no group is a filter guess, ms an envelope guess) [D].
[[nodiscard]] Semantics classify_param(std::string_view name, std::string_view module, std::string_view units);

// True for an output level parameter (spec 03 section 4: volume, gain, level, output), which then joins
// the free set instead of the closed-form gain.
[[nodiscard]] bool is_output_level(std::string_view name, std::string_view module);

// True for a global tuning parameter by name (tune, tuning, fine, pitch; not detune, which is often a
// unison spread) [D].
[[nodiscard]] bool is_pitch_param(std::string_view name);

// True for a seed or "random" parameter, which is pinned at its default (spec 03 section 6).
[[nodiscard]] bool is_seed_param(std::string_view name);

}  // namespace mimicry
