#pragma once
// Failure reports (docs/spec/05_RESILIENCE.md section 7): one JSON file per distinct failure with everything
// `mimicry replay` needs to reproduce it, and what a plugin vendor needs to see it: format, plugin uid and
// version hash, the full parameter vector (ids, names and values), note plan, sample rate, block size,
// outcome, and the minimal failing set once the bisection found it.

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

#include "core/Expected.hpp"
#include "core/Json.hpp"
#include "core/Outcome.hpp"
#include "host/PluginHost.hpp"

namespace mimicry::output {

struct FailureParam {
    std::uint32_t id = 0;
    std::string name;
    double value = 0;      // coordinate u
    std::string text;      // the plugin's display text, when known
};

struct FailureBound {
    std::uint32_t id = 0;
    std::string name;
    double threshold = 0;  // boundary in u between passing and failing (spec 05 section 4)
};

struct FailureReport {
    host::PluginInfo info;
    std::string plugin_path;
    host::Job job;                       // job.coords is the failing vector, in the order of `params`
    std::vector<FailureParam> params;    // id, name and value of every parameter
    Outcome outcome = Outcome::Crash;
    std::string message;
    std::vector<FailureBound> minimal;   // S, empty until bisected

    // Fills params from the parameter table and job.coords.
    void set_params(const std::vector<host::ParamDesc>& table);
    // Hash of format, uid, version, outcome and S (or the vector when S is unknown): distinct failures
    // get distinct files, a repeat overwrites its own.
    [[nodiscard]] std::string hash() const;
    // <plugin name>-<hash>.json, unsafe characters as '_'.
    [[nodiscard]] std::string file_name() const;

    [[nodiscard]] json::Value to_json() const;
    [[nodiscard]] static expected<FailureReport, std::string> from_json(const json::Value& v);
    // Writes <dir>/<file_name()>; returns the path.
    [[nodiscard]] expected<std::filesystem::path, std::string> write(const std::filesystem::path& dir) const;
    [[nodiscard]] static expected<FailureReport, std::string> load(const std::filesystem::path& path);

    // The failing vector on another copy of the parameter table, matched on id AND name (CLAUDE.md rule 7):
    // parameters the report does not know stay at their default; report entries the table lacks go to
    // `missing`.
    [[nodiscard]] std::vector<double> coords_for(const std::vector<host::ParamDesc>& table,
                                                 std::vector<std::string>* missing = nullptr) const;
};

}  // namespace mimicry::output
