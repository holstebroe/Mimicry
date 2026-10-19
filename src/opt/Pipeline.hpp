#pragma once
// The optimisation pipeline (docs/spec/04_OPTIMIZER.md sections 4-7):
//   S1 seed (LHS inside the safe box + fingerprint neighbours + defaults, keep the best K)
//   S2-S4 staged by parameter group (guided / hybrid only)
//   S5 joint polish: CMA-ES (n <= 64) or sep-CMA-ES, R independent restarts
//   S6 categorical outer loop, S7 snap and coordinate refinement
// then the top-K diverse answers. Stops on the time cap, patience, or evaluation budget.

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "opt/Constraints.hpp"
#include "opt/Objective.hpp"
#include "opt/Space.hpp"

namespace mimicry::opt {

struct Progress {
    std::string stage;
    long evals = 0;
    double best = 0;       // best fitness so far (loss + penalties)
    double seconds = 0;
};

struct PipelineOptions {
    double time_cap_s = 1200;     // 20 min [S]
    double patience_s = 240;      // 4 min without improvement [S]
    long max_evals = 0;           // 0 = no budget
    int seed_samples = 400;       // S1 [D]
    int keep_best = 8;            // S1 [D]
    double sigma_stage = 0.3;     // S2-S4 [D]
    double sigma_polish = 0.1;    // S5 [D]
    int sep_threshold = 64;       // sep-CMA-ES above this many parameters [D]
    int restarts = 3;             // R [A]
    int top_k = 3;                // K [A]
    double diversity_param = 0.1; // minimum RMS parameter distance between top-K answers [D]
    double diversity_feature = 0.5;  // minimum embedding distance between top-K answers [D]
    bool prior = true;            // regularisation tie-breaker (spec 04 section 6)
    double prior_fraction = 0.005;
    double boundary_weight = 100; // dB per unit^2 of clip distance [D]
    int max_categorical = 3;      // S6 enumerates at most this many parameters [D] ...
    int max_categorical_values = 4;  // ... with at most this many values each [D]
    double invalid_shrink = 0.7;  // spec 05 section 3: sigma factor when > half a generation is invalid
    // Surrogate pre-screening (spec 04 section 7): auto above this many seconds per candidate; < 0 off,
    // 0 always on.
    double surrogate_above_s = 1.0;
    // Hazard constraints (spec 05 sections 3-4): the set the run tests candidates against, learns into and
    // updates (the caller stores it in the profile). Null: no resampling, no bisection.
    ConstraintSet* constraints = nullptr;
    int resample = 5;             // redraws of a CMA-ES sample inside a constraint before it is rendered anyway [D]
    int max_bisections = 6;       // new failures bisected per run [D]
    BisectOptions bisect;
    int max_failure_examples = 32;  // distinct failures kept for the failure reports (spec 05 section 7) [D]
    // Polled between batches; true stops the run at once (e.g. the plugin's parameter table changed,
    // spec 05 section 9). The result is then marked `aborted`.
    std::function<bool()> abort;
    std::vector<int> stages;      // empty = all of 1..7 (--stages)
    std::uint64_t seed = 1;
    double progress_interval_s = 15;  // [S]
    std::function<void(const Progress&)> progress;
    std::function<void(const std::string&)> log;
};

struct Candidate {
    std::vector<double> coords;  // full vector
    Evaluation eval;
    double fitness = 0;          // loss + boundary penalty + prior (ranked by CMA-ES)
};

// One distinct failure for a failure report (spec 05 section 7): per outcome one example, plus one per
// bisected minimal set.
struct FailureExample {
    std::vector<double> coords;       // the full vector that failed
    Outcome outcome = Outcome::Crash;
    std::vector<std::size_t> minimal; // S from the bisection (parameter indices), empty when not bisected
    std::vector<double> threshold;    // per entry of `minimal`
};

struct StageReport {
    std::string name;
    int free = 0;            // parameters searched
    double before = 0, after = 0;  // best loss (dB) before and after
    long evals = 0;
    double seconds = 0;
};

struct PipelineResult {
    Candidate best;
    std::vector<Candidate> top_k;
    std::vector<StageReport> stages;
    long evals = 0;
    std::map<Outcome, long> outcomes;  // every evaluation's outcome (spec 05 section 7)
    double seconds = 0;
    bool time_capped = false;
    bool surrogate_used = false;
    long resampled = 0;            // samples redrawn because they fell inside a constraint
    long skipped = 0;              // S1 points not rendered because they fell inside a constraint
    int bisections = 0;            // failures bisected
    long bisect_renders = 0;       // renders spent bisecting (not counted in evals)
    std::vector<Constraint> learned;  // constraints found by this run's bisections
    std::vector<FailureExample> failures;
    bool aborted = false;             // options.abort stopped the run
};

// `seeds` are extra full vectors for S1 (fingerprint neighbours, a warm start).
[[nodiscard]] PipelineResult run_pipeline(Objective& objective, const Space& space, const PipelineOptions& options,
                                          const std::vector<std::vector<double>>& seeds = {});

}  // namespace mimicry::opt
