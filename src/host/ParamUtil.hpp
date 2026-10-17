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

[[nodiscard]] double coord_to_plain(const ParamDesc& p, double u);
[[nodiscard]] double plain_to_coord(const ParamDesc& p, double plain);

}  // namespace mimicry::host
