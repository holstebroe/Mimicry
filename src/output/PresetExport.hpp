#pragma once
// Preset files in the portable form (docs/spec/07_OUTPUT_VALIDATION.md section 3): `params.json` with every
// parameter's id, name, module, normalised and displayed value, and the plugin's own state blob.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "core/Expected.hpp"
#include "core/Json.hpp"
#include "host/PluginHost.hpp"

namespace mimicry::output {

// The plugin's display text for a value (Instance::value_text or a worker pool's).
using ValueText = std::function<std::string(std::uint32_t, double)>;

[[nodiscard]] json::Value params_json(const host::PluginInfo& info, const std::vector<host::ParamDesc>& params,
                                      const std::vector<double>& coords, const ValueText& text);

[[nodiscard]] expected<void, std::string> write_bytes(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes);
[[nodiscard]] expected<void, std::string> write_text(const std::filesystem::path& path, const std::string& text);

}  // namespace mimicry::output
