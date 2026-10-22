#pragma once
// Shared pieces of the command line: option parsing, a plugin behind a render batch (worker pool or one
// in-process instance), profile loading.

#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/Expected.hpp"
#include "core/Json.hpp"
#include "host/PluginHost.hpp"
#include "opt/Objective.hpp"
#include "opt/Profile.hpp"
#include "supervisor/WorkerPool.hpp"

namespace mimicry::app {

// --key value and --flag options plus positional arguments.
struct Args {
    std::map<std::string, std::string, std::less<>> opts;
    std::vector<std::string> positional;
    [[nodiscard]] bool has(std::string_view k) const { return opts.count(std::string(k)) != 0; }
    [[nodiscard]] std::string get(std::string_view k, std::string fallback = {}) const;
    [[nodiscard]] double num(std::string_view k, double fallback) const;

    [[nodiscard]] json::Value to_json() const;
    [[nodiscard]] static Args from_json(const json::Value& v);
    // `over` wins: a resumed run keeps its saved options unless the command line gives new ones.
    [[nodiscard]] Args merged_with(const Args& over) const;
};

[[nodiscard]] Args parse_args(int argc, char** argv, int first);
[[nodiscard]] std::vector<std::string> split(const std::string& s);

// A plugin behind a RenderBatch: a worker pool, or (--no-isolation) one in-process instance.
struct Plugin {
    std::unique_ptr<supervisor::WorkerPool> pool;
    std::unique_ptr<host::Instance> local;
    std::mutex local_mutex;
    host::PluginInfo info;
    std::vector<host::ParamDesc> params;
    std::string path;

    [[nodiscard]] opt::RenderBatch batch();
    [[nodiscard]] int workers() const { return pool ? pool->size() : 1; }
    [[nodiscard]] std::string value_text(std::uint32_t id, double u);
};

// --timeout-min S and --timeout-factor F: the per-render watchdog of the workers (WorkerOptions).
[[nodiscard]] supervisor::WorkerOptions worker_options(const Args& a);

[[nodiscard]] expected<std::unique_ptr<Plugin>, std::string> open_plugin(const std::string& path, const Args& args);

[[nodiscard]] std::filesystem::path profiles_dir(const Args& a);
[[nodiscard]] std::optional<opt::Profile> load_profile(const Args& a, const host::PluginInfo& info);
[[nodiscard]] host::Job note_job(const Args& a, int key, double cents, double gate_ms);

// Prints "mimicry <command>: <message>" on stderr, or in events mode a {"event":"done","status":"failed"}.
// Returns 1.
int fail(std::string_view command, const std::string& message);

// The options that steer one invocation, not the run: not saved in run.json (a resume brings its own).
[[nodiscard]] Args without_control_options(Args a);

// "20260102-153045"
[[nodiscard]] std::string timestamp_now();
// Letters, digits, '-' and '_' of a file stem; the rest becomes '_'.
[[nodiscard]] std::string slug(const std::string& text);

}  // namespace mimicry::app
