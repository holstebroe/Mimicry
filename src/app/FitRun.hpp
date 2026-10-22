#pragma once
// One fit of a plugin to an analysed target, with a run directory, events, cancel and resume
// (docs/spec/04_OPTIMIZER.md section 10, spec 09). `mimicry fit` runs one; `mimicry transfer` runs one per
// source preset.

#include <filesystem>
#include <functional>
#include <string>

#include "app/Cli.hpp"
#include "features/Loss.hpp"
#include "opt/Pipeline.hpp"

namespace mimicry::app {

struct FitSpec {
    Plugin* plugin = nullptr;
    const Args* args = nullptr;
    const features::Target* target = nullptr;
    features::AnalyzeOptions analyze;
    std::filesystem::path dir;       // exists; checkpoint.json, best.*, ... go here
    std::string target_label;        // result.json "target": the WAV path, or "<source plugin>: <preset>"
    double default_minutes = 20;     // --minutes when the command line gives none
    bool resume = false;             // continue from dir/checkpoint.json
    double more_minutes = 0;         // resume: extra time on top of the time already used (0: what is left)
    // For events of a bank: this fit is preset `preset_index` of `preset_total` (0: not a bank).
    int preset_index = 0, preset_total = 0;
    std::string preset_name;
    // Called with every progress report (after the interval), e.g. to keep run.json current.
    std::function<void(const opt::Progress&)> on_progress;
    // Extra start points for S1 (a neighbouring preset's best); full vectors.
    std::vector<std::vector<double>> seeds;
};

struct FitOutcome {
    std::string status;              // "finished", "cancelled", "failed"
    std::string error;               // when failed
    opt::PipelineResult result;
    bool have_best = false;
    double loss_db = 0, gain_db = 0, seconds = 0, floor_db = 0;
    long evals = 0;
    double time_cap_s = 0;
};

[[nodiscard]] FitOutcome run_fit(const FitSpec& spec);

}  // namespace mimicry::app
