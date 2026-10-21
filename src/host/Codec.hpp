#pragma once
// JSON encoding of the host types for the supervisor / worker protocol (docs/spec/05_RESILIENCE.md section 1).
// Doubles round-trip exactly, so a job decoded in the worker is the job the supervisor built. A job's state
// blob is not encoded: the worker protocol passes it through the shared memory ("state_bytes").

#include "core/Json.hpp"
#include "host/PluginHost.hpp"

namespace mimicry::host {

[[nodiscard]] json::Value to_json(const Job& job);
[[nodiscard]] expected<Job, std::string> job_from_json(const json::Value& v);

[[nodiscard]] json::Value to_json(const PluginInfo& info);
[[nodiscard]] PluginInfo info_from_json(const json::Value& v);

[[nodiscard]] json::Value to_json(const ParamDesc& p);
[[nodiscard]] ParamDesc param_from_json(const json::Value& v);

// Every field of the ref, so a ref round-trips through index.json and the worker protocol.
[[nodiscard]] json::Value to_json(const PresetRef& ref);
[[nodiscard]] expected<PresetRef, std::string> preset_from_json(const json::Value& v);

[[nodiscard]] expected<Outcome, std::string> outcome_from_string(std::string_view name);

}  // namespace mimicry::host
