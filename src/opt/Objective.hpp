#pragma once
// What the optimizer minimises (docs/spec/04_OPTIMIZER.md section 5): a batch of FULL parameter vectors
// (CLAUDE.md rule 5) in, one Evaluation per vector out, failures included (rule 6, spec 05 section 2).
//
// `AudioObjective` renders a batch through any render function (a WorkerPool in production, an in-process
// Instance only for --no-isolation and tests: rule 4), caches renders (spec 03 section 7), and scores the
// audio against a Target with one Scorer per thread.

#include <atomic>
#include <chrono>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "features/Loss.hpp"
#include "host/PluginHost.hpp"
#include "opt/Embedding.hpp"
#include "opt/RenderCache.hpp"

namespace mimicry::opt {

struct Evaluation {
    Outcome outcome = Outcome::Ok;
    double loss = std::numeric_limits<double>::infinity();  // scorer cost in dB when Ok
    features::Score score;
    std::string message;
    Embedding embedding;  // only when asked for
    [[nodiscard]] bool ok() const noexcept { return outcome == Outcome::Ok; }
};

class Objective {
public:
    virtual ~Objective() = default;
    [[nodiscard]] virtual std::vector<Evaluation> evaluate(const std::vector<std::vector<double>>& coords,
                                                           bool embed = false) = 0;
    // Mean wall time of one candidate (render + score) so far; decides the surrogate (spec 04 section 7).
    [[nodiscard]] virtual double seconds_per_candidate() const { return 0; }
};

// Renders jobs; results in job order. WorkerPool::render_all has this shape.
using RenderBatch = std::function<std::vector<expected<host::Audio, Outcome>>(const std::vector<host::Job>&)>;

// Mono mix of a render (03 section 1).
[[nodiscard]] std::vector<float> mono_of(const host::Audio& audio);

class AudioObjective final : public Objective {
public:
    // `job` is the template (notes, rate, settle, ...); its coords are replaced per candidate. `target`
    // must outlive the objective.
    AudioObjective(RenderBatch render, host::Job job, const features::Target& target, features::Weights weights,
                   features::ScorerOptions options = {}, int threads = 1, std::size_t cache_bytes = std::size_t{256} << 20);

    [[nodiscard]] std::vector<Evaluation> evaluate(const std::vector<std::vector<double>>& coords, bool embed) override;
    [[nodiscard]] double seconds_per_candidate() const override;

    // Renders (cached) one vector; null when the render failed.
    [[nodiscard]] std::shared_ptr<const host::Audio> render(const std::vector<double>& coords, Outcome* outcome = nullptr);
    // Fits the global onset offset (03 section 1) from this vector's render; false if it failed.
    bool calibrate_offset(const std::vector<double>& coords);
    [[nodiscard]] long offset() const;
    // Renders per candidate, averaged (spec 03 section 6); > 1 bypasses the cache for the repeats.
    void set_renders_per_candidate(int n) { repeats_ = std::max(1, n); }

    [[nodiscard]] const host::Job& job() const noexcept { return job_; }
    [[nodiscard]] const features::Target& target() const noexcept { return target_; }
    [[nodiscard]] const RenderCache& cache() const noexcept { return cache_; }

private:
    RenderBatch render_;
    host::Job job_;
    const features::Target& target_;
    std::vector<std::unique_ptr<features::Scorer>> scorers_;
    std::vector<std::unique_ptr<Embedder>> embedders_;
    RenderCache cache_;
    int repeats_ = 1;
    std::atomic<long> candidates_{0};
    std::atomic<long long> nanos_{0};
};

}  // namespace mimicry::opt
