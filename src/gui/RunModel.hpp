#pragma once
// What the monitor shows (docs/spec/09_GUI.md sections 4-5): a model fed with the events of one
// `mimicry --events` process. No windows in here, so it is tested on every platform.

#include <deque>
#include <map>
#include <string>
#include <vector>

#include "core/Json.hpp"

namespace mimicry::gui {

struct StageRow {
    std::string name;
    int free = 0;
    double before_db = 0, after_db = 0, seconds = 0;
    long evals = 0;
    bool finished = false;
};

struct LossPoint {
    double seconds = 0;  // run time, including earlier sessions
    double best_db = 0;
    int preset = 0;      // bank: which preset (the graph shows the current one)
};

// One harvested preset (harvest) ...
struct PresetRow {
    std::string method, name, outcome, message;
    int number = 0, position = 0, of = 0;
};

// ... or one source preset of a transfer.
struct BankRow {
    int index = 0;
    std::string name, status, note;
    double loss_db = 0, seconds = 0;
    long evals = 0;
};

enum class RunState { Idle, Starting, Running, Cancelling, Finished, Cancelled, Failed };
[[nodiscard]] const char* to_string(RunState s) noexcept;

struct RunModel {
    RunState state = RunState::Idle;
    std::string command;      // fit, transfer, harvest, probe, ...
    std::string run_dir;
    std::string library;      // harvest: the folder the library was written to
    std::string plugin, target;
    std::string phase;        // "opening the plugin", "fitting", ...
    std::string stage;        // S1 seed, S5 run 2, ...
    std::string error;
    bool resumed = false, resumable = false;
    int exit_code = -1;

    // Progress of the current fit (or of the harvest's current method).
    double fraction = 0, seconds = 0, time_cap_s = 0, best_db = 0, evals_per_s = 0;
    long evals = 0;
    bool have_best = false;

    // Bank (transfer): preset `preset` of `presets`, `presets_done` finished.
    int preset = 0, presets = 0, presets_done = 0;
    std::string preset_name;

    std::vector<StageRow> stages;     // of the current fit (a new preset starts a new list)
    std::vector<LossPoint> history;   // best loss over run time of the current fit
    std::vector<PresetRow> harvested; // harvest
    std::vector<BankRow> bank;        // transfer
    int harvest_ok = 0;
    int previews = 0;                 // how many live A/B files were announced
    std::map<std::string, std::string> files;  // done event: ab, match, report
    std::deque<std::string> log;      // newest last, capped

    // Feeds one line of the child's output: a JSON event, or plain text for the log.
    void apply_line(const std::string& line, bool from_stderr = false);
    void apply(const json::Value& event);
    // The process ended with this exit code without a done event (a crash): marks the run failed.
    void process_exited(int code);
    void begin(const std::string& command_name);

    // 0..1 over the whole job: a bank counts finished presets plus the fraction of the running one.
    [[nodiscard]] double overall_fraction() const;
    // Seconds left, or -1 when it cannot be said yet.
    [[nodiscard]] double eta_seconds() const;
    [[nodiscard]] bool active() const noexcept {
        return state == RunState::Starting || state == RunState::Running || state == RunState::Cancelling;
    }
    // A one-line summary for the status bar.
    [[nodiscard]] std::string status_line() const;
};

[[nodiscard]] std::string format_duration(double seconds);

}  // namespace mimicry::gui
