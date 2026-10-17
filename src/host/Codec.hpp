#pragma once
// JSON encoding of the host types for the supervisor / worker protocol (docs/spec/05_RESILIENCE.md section 1).
// Doubles round-trip exactly, so a job decoded in the worker is the job the supervisor built.

#include "core/Json.hpp"
#include "host/PluginHost.hpp"

namespace mimicry::host {

[[nodiscard]] json::Value to_json(const Job& job);
[[nodiscard]] expected<Job, std::string> job_from_json(const json::Value& v);

[[nodiscard]] json::Value to_json(const PluginInfo& info);
[[nodiscard]] PluginInfo info_from_json(const json::Value& v);

[[nodiscard]] json::Value to_json(const ParamDesc& p);
[[nodiscard]] ParamDesc param_from_json(const json::Value& v);

[[nodiscard]] expected<Outcome, std::string> outcome_from_string(std::string_view name);

}  // namespace mimicry::host
