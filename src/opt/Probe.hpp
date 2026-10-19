#pragma once
// Sensitivity probe (docs/spec/04_OPTIMIZER.md section 3, `mimicry probe`): measures what every parameter
// does, which are dead, a safe box per parameter (05 section 3), and which pairs interact. The result is
// the plugin profile. Format-independent: renders go through a RenderBatch (a worker pool in production).

#include <functional>
#include <string>

#include "core/Expected.hpp"
#include "opt/Objective.hpp"
#include "opt/Profile.hpp"

namespace mimicry::opt {

// Scalar descriptors of one render that the classes are read from (spec 04 section 3, step 2).
struct Descriptor {
    double centroid_oct = 0;  // log2 of the mean spectral centroid
    double f0 = 0;            // Hz, 0 when not pitched
    double decay_ms = 0;      // peak to -20 dB
    double tail_ms = 0;       // onset to -60 dB
    double flatness_db = 0;   // noisiness
    double level_db = -120;
    Embedding embedding;
};

[[nodiscard]] Descriptor describe(const std::vector<float>& mono, int sample_rate, Embedder& embedder);

struct ProbeOptions {
    host::Job job;                // note plan template (coords are set per render)
    bool pairs = true;            // 2x2 interaction probes (step 5)
    int pair_params = 10;         // strongest parameters whose pairs are probed [D]
    double pair_lo = 0.25, pair_hi = 0.75;  // probe values of the 2x2 design [D]
    std::function<void(const std::string&)> log;
};

[[nodiscard]] expected<Profile, std::string> probe(const RenderBatch& render, const host::PluginInfo& info,
                                                   const std::vector<host::ParamDesc>& params, const ProbeOptions& options);

}  // namespace mimicry::opt
