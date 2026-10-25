#pragma once
// Supervisor side of the worker protocol (docs/spec/05_RESILIENCE.md section 1). A WorkerClient looks
// like a plugin Instance, but every call runs in a separate mimicry_worker process: a crash, a hang or a
// poisoned state costs the candidate an outcome (Crash / Timeout), never the supervisor.

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include "core/Ipc.hpp"
#include "core/Json.hpp"
#include "host/PluginHost.hpp"

namespace mimicry::supervisor {

struct WorkerOptions {
    std::filesystem::path worker_exe;   // default: mimicry_worker next to the running executable
    double min_timeout_s = 5.0;         // per-job wall clock: max(min_timeout_s, factor x audio duration incl. tail) [D]
    double timeout_factor = 20.0;
    double open_timeout_s = 60.0;       // loading a plugin can be slow
    double call_timeout_s = 30.0;       // state, value text, reset
    std::size_t shm_bytes = 128u << 20; // audio and state exchange area
    int recycle_after = 500;            // fresh process every N renders, bounds leaks and drift (spec 05 section 5)
    // After a failed render, render the last job that passed (the canary); if it fails too, the plugin is
    // poisoned beyond a reset and the worker restarts (spec 05 section 5).
    bool canary = true;
    double canary_tail_ms = 250.0;      // the canary's tail is cut to this [A]
};

struct WorkerStats {
    std::uint64_t spawned = 0;
    std::uint64_t crashes = 0;
    std::uint64_t timeouts = 0;
    std::uint64_t recycled = 0;
    std::uint64_t canaries = 0;         // canary renders after a failure
    std::uint64_t canary_restarts = 0;  // canaries that failed: worker restarted
};

class WorkerClient final : public host::Instance {
public:
    // Starts a worker and opens the plugin in it.
    [[nodiscard]] static expected<std::unique_ptr<WorkerClient>, host::HostError> open(
        const std::filesystem::path& plugin, std::string_view uid = {}, WorkerOptions options = {});
    ~WorkerClient() override;

    [[nodiscard]] const host::PluginInfo& info() const override { return info_; }
    [[nodiscard]] const std::vector<host::ParamDesc>& parameters() const override { return params_; }
    [[nodiscard]] std::string value_text(std::uint32_t id, double coord) const override;
    [[nodiscard]] expected<host::Audio, Outcome> render(const host::Job& job) override;
    [[nodiscard]] const std::string& last_message() const override { return message_; }
    [[nodiscard]] int tail_samples() const override { return tail_; }
    [[nodiscard]] int latency_samples() const override { return latency_; }
    [[nodiscard]] std::uint64_t parameters_version() const override { return version_; }
    void pump() override {}  // the worker pumps its own main thread

    [[nodiscard]] expected<std::vector<std::uint8_t>, host::HostError> save_state() override;
    [[nodiscard]] expected<void, host::HostError> load_state(const std::vector<std::uint8_t>& blob) override;
    [[nodiscard]] expected<void, host::HostError> reset() override;
    // The editor opens in the worker (the plugin never runs in this process, rule 4); the worker pumps its
    // window while it waits for the next command.
    using host::Instance::open_editor;  // the form with a parent window is for in-process instances
    [[nodiscard]] expected<void, host::HostError> open_editor() override;
    void close_editor() override;
    [[nodiscard]] expected<std::vector<host::PresetRef>, host::HostError> presets() override;
    // A crash or hang while loading costs the worker (restarted on the next call); the error says which.
    [[nodiscard]] expected<host::CapturedPreset, host::HostError> apply_preset(const host::PresetRef& ref, double wait_ms = 200) override;

    // The editor session of a capture (Windows workers, docs/spec/06_TARGETS.md section 6): the worker opens the
    // plugin's editor in a window of its own and keeps the captures; the supervisor polls and takes them. A lost
    // worker ends the session.
    struct EditorStatus {
        bool open = false, teaching = false, taught = false, auto_running = false;
        int captures = 0;
        int signals = 0;                  // preset-change signals since the last poll
        std::vector<std::string> events;  // what happened since the last poll ("captured 3", "closed", ...)
        json::Value click;                // the taught click, null before
    };
    using host::Instance::editor_open;
    [[nodiscard]] expected<host::EditorSize, host::HostError> editor_open(const std::string& title, const json::Value& click = {});
    [[nodiscard]] expected<EditorStatus, host::HostError> editor_poll();
    // Capture `index` of the session; `hash` receives its state hash.
    [[nodiscard]] expected<host::CapturedPreset, host::HostError> editor_take(int index, std::string* hash = nullptr);
    // "capture" (options: name), "teach", "auto" (max, wait_ms), "stop", "done".
    [[nodiscard]] expected<void, host::HostError> editor_action(const std::string& action, const json::Value& options = {});
    void editor_close();

    [[nodiscard]] const WorkerStats& stats() const { return stats_; }
    // Non-empty once the plugin's parameter table differs from the one it had when first opened (a restart
    // request or a re-open renamed, added or removed parameters). Every render then fails with HostError:
    // a run must stop rather than map stale ids (spec 05 section 9).
    [[nodiscard]] const std::string& table_change() const { return table_change_; }
    // Test hook: the worker's process id, 0 if none is running.
    [[nodiscard]] long worker_pid() const { return process_.id() > 0 ? process_.id() : 0; }

private:
    WorkerClient() = default;

    [[nodiscard]] expected<void, host::HostError> start_worker();
    void stop_worker();
    // Sends one request and waits. `error` is set to "timeout" / "crash: ..." when the worker had to go.
    enum class Call { Ok, Timeout, Crashed };
    Call call(const json::Value& request, double timeout_s, json::Value& reply, std::string& detail);
    void adopt(const json::Value& description);
    [[nodiscard]] expected<host::Audio, Outcome> render_once(const host::Job& job);
    // Blocks the worker started in the current render (the progress word in the shared memory).
    [[nodiscard]] std::uint64_t blocks_done() const;
    [[nodiscard]] std::size_t payload() const { return shm_.size() > ipc::kShmProgressBytes ? shm_.size() - ipc::kShmProgressBytes : 0; }

    std::filesystem::path plugin_;
    std::string uid_;
    WorkerOptions options_;
    ipc::LineChannel channel_;
    ipc::SharedMemory shm_;
    ipc::Process process_;
    int renders_since_start_ = 0;
    // True once a plugin is open in the current worker. After a kill the next call restarts one.
    bool open_ = false;

    host::PluginInfo info_;
    std::vector<host::ParamDesc> params_;    // the table of the first open: what callers index into
    bool have_params_ = false;
    std::string table_change_;
    std::optional<host::Job> canary_;
    int tail_ = 0, latency_ = 0;
    std::uint64_t version_ = 1;
    std::string message_;
    WorkerStats stats_;
};

}  // namespace mimicry::supervisor
