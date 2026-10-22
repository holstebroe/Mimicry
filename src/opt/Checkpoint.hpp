#pragma once
// Pipeline checkpoint (docs/spec/04_OPTIMIZER.md section 10): what a stopped run needs to continue later.
// Not the CMA-ES internals: a stage that was interrupted starts again from the best point, a stage that
// finished is skipped. Coordinates are full vectors (CLAUDE.md rule 5); candidates are re-scored on resume,
// so the file holds no audio features.

#include <filesystem>
#include <map>
#include <vector>

#include "core/Expected.hpp"
#include "core/Json.hpp"
#include "core/Outcome.hpp"

namespace mimicry::opt {

struct StageReport {
    std::string name;
    int free = 0;            // parameters searched
    double before = 0, after = 0;  // best loss (dB) before and after
    long evals = 0;
    double seconds = 0;
};

struct PipelineState {
    int version = 1;
    std::size_t dimension = 0;               // parameter count the vectors belong to
    std::vector<int> done;                   // stages that ran to their own end: 1 (S1), 2-4 (groups), 5, 6, 7
    std::vector<double> best;                // best full vector so far (empty before the first valid one)
    double best_loss = 0;                    // for display only; re-scored on resume
    std::vector<std::vector<double>> kept;       // S1's best K, starts of the S5 restarts
    std::vector<std::vector<double>> finalists;  // candidates for the top-K set
    double elapsed_s = 0;                    // run time used so far, summed over all sessions
    long evals = 0;
    int sessions = 1;                        // how many times this run was started (resumes + 1)
    std::vector<StageReport> stages;
    std::map<Outcome, long> outcomes;
    long resampled = 0, skipped = 0, bisect_renders = 0;
    int bisections = 0;

    [[nodiscard]] bool is_done(int stage) const;
    [[nodiscard]] json::Value to_json() const;
    [[nodiscard]] static expected<PipelineState, std::string> from_json(const json::Value& v);
    [[nodiscard]] expected<void, std::string> save(const std::filesystem::path& path) const;
    [[nodiscard]] static expected<PipelineState, std::string> load(const std::filesystem::path& path);
};

}  // namespace mimicry::opt
