#include "gui/RunModel.hpp"

#include <algorithm>
#include <cmath>
#include <format>

namespace mimicry::gui {

const char* to_string(RunState s) noexcept {
    switch (s) {
        case RunState::Idle: return "idle";
        case RunState::Starting: return "starting";
        case RunState::Running: return "running";
        case RunState::Cancelling: return "cancelling";
        case RunState::Finished: return "finished";
        case RunState::Cancelled: return "cancelled";
        case RunState::Failed: return "failed";
    }
    return "?";
}

std::string format_duration(double seconds) {
    if (!(seconds >= 0) || !std::isfinite(seconds)) return "-";
    const long s = long(seconds + 0.5);
    if (s >= 3600) return std::format("{}:{:02}:{:02}", s / 3600, (s / 60) % 60, s % 60);
    return std::format("{}:{:02}", s / 60, s % 60);
}

void RunModel::begin(const std::string& command_name) {
    *this = RunModel{};
    command = command_name;
    state = RunState::Starting;
}

void RunModel::apply_line(const std::string& line, bool from_stderr) {
    if (!line.empty() && line.front() == '{' && !from_stderr) {
        if (auto v = json::parse(line); v && v->is_object()) {
            apply(*v);
            return;
        }
    }
    if (!line.empty()) {
        log.push_back(from_stderr ? "! " + line : line);
        while (log.size() > 3000) log.pop_front();
    }
}

void RunModel::apply(const json::Value& e) {
    const std::string kind = e["event"].as_string();
    if (kind == "start") {
        command = e["command"].as_string();
        run_dir = e["run_dir"].as_string();
        plugin = e["plugin"].as_string();
        target = e["target"].as_string();
        resumed = e["resumed"].as_bool();
        if (state == RunState::Starting) state = RunState::Running;
    } else if (kind == "phase") {
        phase = e["name"].as_string();
        if (e["time_cap_s"].is_number()) time_cap_s = e["time_cap_s"].as_number();
        if (e["preset"].is_number()) {
            preset = int(e["preset"].as_number());
            presets = int(e["presets"].as_number());
        }
    } else if (kind == "progress") {
        stage = e["stage"].as_string();
        evals = long(e["evals"].as_number());
        seconds = e["seconds"].as_number();
        best_db = e["best_db"].as_number();
        have_best = std::isfinite(best_db) && best_db < 1e5;
        fraction = e["fraction"].as_number();
        evals_per_s = e["evals_per_s"].as_number();
        if (e["time_cap_s"].is_number()) time_cap_s = e["time_cap_s"].as_number();
        if (e["preset"].is_number()) {
            const int p = int(e["preset"].as_number());
            if (p != preset) {  // the next preset: a new curve and stage list
                history.clear();
                stages.clear();
            }
            preset = p;
            presets = int(e["presets"].as_number());
            preset_name = e["preset_name"].as_string();
        }
        if (have_best) history.push_back({seconds, best_db, preset});
        if (state == RunState::Starting) state = RunState::Running;
    } else if (kind == "preview") {  // a fresh A/B file of the best so far
        files["ab"] = e["ab"].as_string();
        ++previews;
    } else if (kind == "stage") {
        const std::string name = e["name"].as_string();
        const bool finished = e["finished"].as_bool();
        if (e["preset"].is_number() && int(e["preset"].as_number()) != preset) {
            preset = int(e["preset"].as_number());
            history.clear();
            stages.clear();
        }
        StageRow* row = nullptr;
        for (auto it = stages.rbegin(); it != stages.rend(); ++it)
            if (it->name == name && !it->finished) {
                row = &*it;
                break;
            }
        if (!row) {
            stages.emplace_back();
            row = &stages.back();
            row->name = name;
        }
        row->free = int(e["free"].as_number());
        row->before_db = e["before_db"].as_number();
        row->finished = finished;
        if (finished) {
            row->after_db = e["after_db"].as_number();
            row->evals = long(e["evals"].as_number());
            row->seconds = e["seconds"].as_number();
        }
    } else if (kind == "preset") {  // harvest
        PresetRow r;
        r.method = e["method"].as_string();
        r.name = e["name"].as_string();
        r.outcome = e["outcome"].as_string();
        r.message = e["message"].as_string();
        r.number = int(e["number"].as_number());
        r.position = int(e["position"].as_number());
        r.of = int(e["of"].as_number());
        harvest_ok = int(e["ok"].as_number());
        if (r.of > 0) fraction = double(r.position) / double(r.of);
        harvested.push_back(std::move(r));
    } else if (kind == "preset_start") {
        preset = int(e["preset"].as_number());
        presets = int(e["presets"].as_number());
        preset_name = e["name"].as_string();
        history.clear();
        stages.clear();
        fraction = 0;
        if (std::size_t(preset) > bank.size()) bank.resize(std::size_t(preset));
        auto& row = bank[std::size_t(preset - 1)];
        row.index = preset;
        row.name = preset_name;
        row.status = "running";
    } else if (kind == "preset_done") {
        const int i = int(e["preset"].as_number());
        if (i > 0) {
            if (std::size_t(i) > bank.size()) bank.resize(std::size_t(i));
            auto& row = bank[std::size_t(i - 1)];
            row.index = i;
            row.name = e["name"].as_string();
            row.status = e["status"].as_string();
            row.loss_db = e["loss_db"].as_number();
            row.evals = long(e["evals"].as_number());
            row.seconds = e["seconds"].as_number();
            row.note = e["note"].as_string();
        }
    } else if (kind == "bank") {
        presets = int(e["presets"].as_number());
        presets_done = int(e["done"].as_number());
        if (bank.size() < std::size_t(presets)) bank.resize(std::size_t(presets));
        for (std::size_t i = 0; i < bank.size(); ++i)
            if (bank[i].index == 0) {
                bank[i].index = int(i + 1);
                bank[i].status = "pending";
            }
    } else if (kind == "log") {
        const std::string level = e["level"].as_string();
        const std::string text = e["text"].as_string();
        log.push_back(level == "warn" ? "note: " + text : text);
        while (log.size() > 3000) log.pop_front();
    } else if (kind == "done") {
        const std::string status = e["status"].as_string();
        state = status == "finished" ? RunState::Finished : status == "cancelled" ? RunState::Cancelled : RunState::Failed;
        if (e["run_dir"].is_string()) run_dir = e["run_dir"].as_string();
        if (e["library"].is_string()) library = e["library"].as_string();
        resumable = e["resumable"].as_bool();
        if (e["error"].is_string()) error = e["error"].as_string();
        if (e["loss_db"].is_number()) {
            best_db = e["loss_db"].as_number();
            have_best = true;
        }
        if (e["presets_done"].is_number()) presets_done = int(e["presets_done"].as_number());
        for (const auto& [k, v] : e["files"].as_object()) files[k] = v.as_string();
        if (state == RunState::Finished) fraction = 1.0;
        phase = status;
    }
}

void RunModel::process_exited(int code) {
    exit_code = code;
    if (state == RunState::Finished || state == RunState::Cancelled || state == RunState::Failed) return;
    // No done event: the process died, or was ended by the front end.
    state = RunState::Failed;
    if (error.empty()) error = std::format("the process ended unexpectedly (exit code {})", code);
}

double RunModel::overall_fraction() const {
    if (state == RunState::Finished) return 1.0;
    if (presets > 0 && command == "transfer") {
        const double cur = std::clamp(fraction, 0.0, 1.0);
        return std::clamp((double(presets_done) + (preset > presets_done ? cur : 0.0)) / double(presets), 0.0, 1.0);
    }
    return std::clamp(fraction, 0.0, 1.0);
}

double RunModel::eta_seconds() const {
    if (!active()) return -1;
    const double f = overall_fraction();
    if (command == "fit") {
        if (!(time_cap_s > 0) || !(seconds > 0)) return -1;
        double eta = time_cap_s - seconds;  // an upper bound: patience or the evaluation budget may end it sooner
        if (f >= 0.02) eta = std::min(eta, seconds / f - seconds);
        return std::max(0.0, eta);
    }
    if (f < 0.02) return -1;
    // Bank and harvest: elapsed work so far over the fraction done. Elapsed is the sum of what the presets took.
    double spent = seconds;
    for (const auto& b : bank) spent += (b.status == "done" || b.status == "failed") ? b.seconds : 0.0;
    if (command == "harvest") return -1;
    if (spent <= 0) return -1;
    return std::max(0.0, spent / f - spent);
}

std::string RunModel::status_line() const {
    switch (state) {
        case RunState::Idle: return "Ready.";
        case RunState::Starting: return "Starting...";
        case RunState::Cancelling: return "Cancelling: finishing the batch in flight, then saving the results...";
        case RunState::Cancelled: return resumable ? "Cancelled. The results so far are saved; the run can be resumed." : "Cancelled.";
        case RunState::Failed: return "Failed: " + (error.empty() ? std::string("see the log") : error);
        case RunState::Finished:
            if (command == "harvest") return std::format("Finished: {} presets harvested.", harvest_ok);
            if (command == "transfer") return std::format("Finished: {} of {} presets.", presets_done, presets);
            if (have_best) return std::format("Finished: {:.3f} dB.", best_db);
            return "Finished.";
        case RunState::Running: break;
    }
    std::string s = phase.empty() ? std::string("Running") : phase;
    if (command == "transfer" && presets > 0)
        s = std::format("Preset {}/{}{}: {}", std::max(preset, 1), presets, preset_name.empty() ? "" : " (" + preset_name + ")", stage.empty() ? phase : stage);
    else if (!stage.empty() && command == "fit")
        s = stage;
    if (have_best && (command == "fit" || command == "transfer")) s += std::format("   best {:.3f} dB", best_db);
    if (command == "harvest") s += std::format("   {} ok", harvest_ok);
    return s;
}

}  // namespace mimicry::gui
