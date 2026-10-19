#pragma once
// Format-independent parameter helpers: units from value text, the log/linear decision and the
// coordinate <-> plain mapping (docs/spec/02_HOSTING.md section 1, 04_OPTIMIZER.md section 1).

#include <span>
#include <string>

#include "host/PluginHost.hpp"

namespace mimicry::host {

// The unit suffix shared by the value texts at 0..100 % ("12.5 Hz" -> "Hz"), or "" when there is none
// or the texts disagree (enum names, plain numbers).
[[nodiscard]] std::string units_from_texts(std::span<const std::string> texts);

// Log decode when the units say Hz/ms/s or the range spans at least a factor 20 above zero [D].
// Stepped parameters are never log.
[[nodiscard]] bool use_log_scale(double min, double max, int steps, const std::string& units);

// A continuous parameter whose own value texts read as a frequency or a time (Hz/kHz, ms/s, mixed units
// allowed) and run LINEARLY across the coordinate over at least two decades: returns max/min of the texts,
// else 0. Such a parameter is searched with an exact log warp of that ratio (spec 04 section 9, plan OQ9):
// clap-wrapper and many VST3 plugins expose 20 Hz - 20 kHz or 1 ms - 2 s linearly in the normalised value [A].
[[nodiscard]] double linear_wide_taper_ratio(const ParamDesc& p);

// CLAP has no program-change flag, and wrappers do not always set VST3's: a stepped parameter named like
// one is pinned all the same [I].
[[nodiscard]] bool looks_like_program_param(const std::string& name);

[[nodiscard]] double coord_to_plain(const ParamDesc& p, double u);
[[nodiscard]] double plain_to_coord(const ParamDesc& p, double plain);

}  // namespace mimicry::host
