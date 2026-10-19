#include "opt/Probe.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>

#include "core/ParamSemantics.hpp"
#include "features/Features.hpp"
#include "features/Loss.hpp"
#include "features/Pitch.hpp"

namespace mimicry::opt {

namespace {

constexpr double kProbeU[5] = {0.0, 0.25, 0.5, 0.75, 1.0};

// Thresholds that make one unit of each normalised score [D, tune on real plugins (plan OQ3)].
constexpr double kPitchCents = 20, kBrightOct = 0.15, kTimeLog2 = 0.25, kLevelDb = 1.5;

bool probed(const host::ParamDesc& p) {
    return !p.flags.readonly && !p.flags.bypass && !p.flags.program_change && !p.flags.hidden && p.flags.automatable &&
           p.steps != 1;
}

double snap(const host::ParamDesc& p, double u) {
    return p.steps >= 2 ? std::round(u * (p.steps - 1)) / double(p.steps - 1) : u;
}

struct Scores {
    double pitch = 0, bright = 0, time = 0, tail = 0, level = 0, flat = 0, emb = 0;
};

// True when two f0 estimates differ by a small-integer ratio (an octave or harmonic jump of the estimator,
// as when a filter makes another partial the strongest), which is not a pitch change.
bool harmonic_jump(double f0a, double f0b) {
    const double cents = 1200.0 * std::log2(f0b / f0a);
    for (int p = 1; p <= 8; ++p)
        for (int q = 1; q <= 8; ++q)
            if (p != q && std::abs(cents - 1200.0 * std::log2(double(p) / q)) < 10.0) return true;
    return false;
}

Scores compare(const Descriptor& a, const Descriptor& b) {
    Scores s;
    if (a.f0 > 0 && b.f0 > 0 && !harmonic_jump(a.f0, b.f0)) s.pitch = std::abs(1200.0 * std::log2(b.f0 / a.f0)) / kPitchCents;
    s.bright = std::abs(b.centroid_oct - a.centroid_oct) / kBrightOct;
    s.time = std::abs(std::log2((b.decay_ms + 5) / (a.decay_ms + 5))) / kTimeLog2;
    s.tail = std::abs(std::log2((b.tail_ms + 5) / (a.tail_ms + 5))) / kTimeLog2;
    s.level = std::abs(b.level_db - a.level_db) / kLevelDb;
    s.flat = std::abs(b.flatness_db - a.flatness_db);
    s.emb = embedding_distance(a.embedding, b.embedding);
    return s;
}

}  // namespace

Descriptor describe(const std::vector<float>& mono, int rate, Embedder& embedder) {
    Descriptor d;
    d.embedding = embedder.embed(mono);
    d.centroid_oct = d.embedding[kEmbeddingCentroid] / 6.0;
    d.flatness_db = d.embedding[kEmbeddingFlatness];
    d.level_db = d.embedding[kEmbeddingLevel];
    const auto pitch = features::estimate_f0(mono, rate);
    if (pitch.confidence >= 0.5) d.f0 = pitch.f0;
    const std::size_t hop = std::max<std::size_t>(1, std::size_t(rate / 100));
    const auto env = features::rms_envelope_db(mono, hop, hop);
    if (env.empty()) return d;
    const auto peak_it = std::max_element(env.begin(), env.end());
    const float peak = *peak_it;
    if (peak <= features::kFloorDb) return d;
    const std::size_t p = std::size_t(peak_it - env.begin());
    std::size_t f = p;
    while (f < env.size() && env[f] > peak - 20) ++f;
    d.decay_ms = double(f - p) * 10.0;
    const long on = std::max(0L, features::onset_frame(env));
    std::size_t last = std::size_t(on);
    for (std::size_t i = std::size_t(on); i < env.size(); ++i)
        if (env[i] >= peak - 60) last = i;
    d.tail_ms = double(last - std::size_t(on)) * 10.0;
    return d;
}

expected<Profile, std::string> probe(const RenderBatch& render, const host::PluginInfo& info,
                                     const std::vector<host::ParamDesc>& params, const ProbeOptions& options) {
    auto log = [&](const std::string& s) {
        if (options.log) options.log(s);
    };
    Profile prof;
    prof.format = info.format;
    prof.uid = info.uid;
    prof.name = info.name;
    prof.vendor = info.vendor;
    prof.version_hash = info.version_hash;
    const int rate = options.job.sample_rate;
    Embedder embedder(rate);
    std::vector<double> defaults;
    for (const auto& p : params) defaults.push_back(std::clamp(p.default_coord, 0.0, 1.0));

    long renders = 0;
    double render_s = 0;
    auto run = [&](const std::vector<host::Job>& jobs) {
        const auto t0 = std::chrono::steady_clock::now();
        auto r = render(jobs);
        render_s += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        renders += long(jobs.size());
        r.resize(jobs.size(), unexpected(Outcome::HostError));
        return r;
    };
    auto job_at = [&](const std::vector<double>& coords) {
        host::Job j = options.job;
        j.coords = coords;
        return j;
    };

    // 1. Defaults, twice: the repeatability floor (spec 03 section 6).
    auto base = run({job_at(defaults), job_at(defaults)});
    if (!base[0] || !base[1])
        return unexpected(std::format("the plugin's defaults do not render ({})",
                                      to_string(!base[0] ? base[0].error() : base[1].error())));
    const auto mono_a = mono_of(*base[0]), mono_b = mono_of(*base[1]);
    const Descriptor def = describe(mono_a, rate, embedder);
    const Descriptor def2 = describe(mono_b, rate, embedder);
    const auto weights = features::Weights::preset(def.f0 > 0 ? features::SampleKind::Note : features::SampleKind::Texture);
    prof.floor_db = features::repeatability_floor_db(mono_a, mono_b, rate, weights);
    prof.embedding_floor = embedding_distance(def.embedding, def2.embedding);
    const double dead_thr = std::max(3.0 * prof.embedding_floor, 0.15);
    log(std::format("defaults: floor {:.2f} dB, embedding floor {:.3f}, f0 {:.1f} Hz", prof.floor_db,
                    prof.embedding_floor, def.f0));

    // 2. Each parameter at 0, 25, 50, 75, 100 %, others at default.
    std::vector<std::size_t> idx;
    std::vector<host::Job> jobs;
    for (std::size_t i = 0; i < params.size(); ++i) {
        if (!probed(params[i])) continue;
        idx.push_back(i);
        for (double u : kProbeU) {
            auto c = defaults;
            c[i] = snap(params[i], u);
            jobs.push_back(job_at(c));
        }
    }
    const auto results = run(jobs);

    for (std::size_t n = 0; n < idx.size(); ++n) {
        const auto& p = params[idx[n]];
        ParamProfile q;
        q.id = p.id;
        q.name = p.name;
        q.names = classify_param(p.name, p.module, p.units);
        std::array<Scores, 5> sc{};
        std::array<Descriptor, 5> ds{};
        bool all_ok = true;
        for (std::size_t k = 0; k < 5; ++k) {
            const auto& r = results[n * 5 + k];
            q.outcomes.push_back(r ? Outcome::Ok : r.error());
            if (!r) {
                all_ok = false;
                continue;
            }
            ds[k] = describe(mono_of(*r), rate, embedder);
            sc[k] = compare(def, ds[k]);
        }
        Scores mx;
        for (std::size_t k = 0; k < 5; ++k) {
            if (q.outcomes[k] != Outcome::Ok) continue;
            mx.pitch = std::max(mx.pitch, sc[k].pitch);
            mx.bright = std::max(mx.bright, sc[k].bright);
            mx.time = std::max(mx.time, sc[k].time);
            mx.tail = std::max(mx.tail, sc[k].tail);
            mx.level = std::max(mx.level, sc[k].level);
            mx.flat = std::max(mx.flat, sc[k].flat);
            mx.emb = std::max(mx.emb, sc[k].emb);
        }
        q.effect = mx.emb / dead_thr;
        // 3. Classify. Dead: nothing above the floor and nothing failed.
        if (all_ok && mx.emb <= dead_thr && mx.level * kLevelDb <= 0.5 && mx.pitch * kPitchCents <= 5) {
            q.measured = ProbeClass::Dead;
        } else {
            struct Cand {
                ProbeClass c;
                double s;
            };
            const double time = std::max(mx.time, mx.tail);
            const bool fx_like = mx.tail > 2 * mx.time && mx.flat >= 1.5;  // tail grows, noisier: reverb/delay
            const Cand cands[] = {{ProbeClass::Pitch, mx.pitch},
                                  {ProbeClass::Brightness, mx.bright},
                                  {fx_like ? ProbeClass::Fx : ProbeClass::Time, time},
                                  {ProbeClass::Level, mx.level}};
            const Cand* best = &cands[0];
            for (const auto& c : cands)
                if (c.s > best->s) best = &c;
            q.measured = best->s >= 1.0 ? best->c : ProbeClass::Spectrum;
            // Response shape of the dominant descriptor (log vs linear, spec 04 section 3).
            if (all_ok && q.measured != ProbeClass::Spectrum) {
                std::array<double, 5> v{};
                for (std::size_t k = 0; k < 5; ++k) {
                    switch (q.measured) {
                        case ProbeClass::Pitch: v[k] = ds[k].f0 > 0 ? 1200 * std::log2(ds[k].f0) : 0; break;
                        case ProbeClass::Brightness: v[k] = ds[k].centroid_oct; break;
                        case ProbeClass::Level: v[k] = ds[k].level_db; break;
                        default: v[k] = std::log2(std::max(ds[k].decay_ms, ds[k].tail_ms) + 5); break;
                    }
                }
                const double total = std::abs(v[4] - v[0]);
                if (total > 1e-9 && p.steps == 0) {
                    if (std::abs(v[1] - v[0]) > 0.5 * total) q.response = Response::Log;
                    else if (std::abs(v[4] - v[3]) > 0.5 * total) q.response = Response::Exp;
                }
            }
        }
        // 4. Safe box: the run of good probe values around the default, cut at the last good value
        // (spec 05 section 3).
        std::size_t d = 0;
        for (std::size_t k = 1; k < 5; ++k)
            if (std::abs(kProbeU[k] - defaults[idx[n]]) < std::abs(kProbeU[d] - defaults[idx[n]])) d = k;
        if (q.outcomes[d] == Outcome::Ok) {
            std::size_t lo = d, hi = d;
            while (lo > 0 && q.outcomes[lo - 1] == Outcome::Ok) --lo;
            while (hi < 4 && q.outcomes[hi + 1] == Outcome::Ok) ++hi;
            q.safe_lo = std::min(kProbeU[lo], defaults[idx[n]]);
            q.safe_hi = std::max(kProbeU[hi], defaults[idx[n]]);
        }
        prof.params.push_back(std::move(q));
    }

    // 5. Pairwise interaction of the strongest parameters [S: Acidus 2x2 probes].
    if (options.pairs) {
        std::vector<std::size_t> strong;  // indices into prof.params
        for (std::size_t n = 0; n < prof.params.size(); ++n)
            if (!prof.params[n].dead()) strong.push_back(n);
        std::stable_sort(strong.begin(), strong.end(),
                         [&](std::size_t a, std::size_t b) { return prof.params[a].effect > prof.params[b].effect; });
        if (strong.size() > std::size_t(options.pair_params)) strong.resize(std::size_t(options.pair_params));
        std::vector<std::pair<std::size_t, std::size_t>> pairs;
        std::vector<host::Job> pj;
        for (std::size_t x = 0; x < strong.size(); ++x)
            for (std::size_t y = x + 1; y < strong.size(); ++y) {
                const std::size_t a = idx[strong[x]], b = idx[strong[y]];
                pairs.emplace_back(a, b);
                for (int corner = 0; corner < 4; ++corner) {
                    auto c = defaults;
                    c[a] = snap(params[a], (corner & 1) ? options.pair_hi : options.pair_lo);
                    c[b] = snap(params[b], (corner & 2) ? options.pair_hi : options.pair_lo);
                    pj.push_back(job_at(c));
                }
            }
        const auto pr = run(pj);
        const double sep_thr = 2.0 * prof.embedding_floor + 0.5;  // [D]
        for (std::size_t n = 0; n < pairs.size(); ++n) {
            PairProfile q;
            q.a = params[pairs[n].first].id;
            q.b = params[pairs[n].second].id;
            std::array<Embedding, 4> y;
            bool ok = true;
            for (std::size_t k = 0; k < 4; ++k) {
                const auto& r = pr[n * 4 + k];
                if (!r) {
                    ok = false;
                    break;
                }
                y[k] = embedder.embed(mono_of(*r));
            }
            if (!ok) {
                q.interaction = 1e9;  // a corner fails: keep the pair together
                q.separable = false;
            } else {
                // I = (y[A1,B1] - y[A0,B1]) - (y[A1,B0] - y[A0,B0]); corners: bit 0 = A, bit 1 = B.
                double s = 0;
                for (std::size_t i = 0; i < kEmbeddingDims; ++i) {
                    const double v = (double(y[3][i]) - y[2][i]) - (double(y[1][i]) - y[0][i]);
                    s += v * v;
                }
                q.interaction = std::sqrt(s / double(kEmbeddingDims));
                q.separable = q.interaction <= sep_thr;
            }
            prof.pairs.push_back(q);
        }
        log(std::format("pairs: {} probed", pairs.size()));
    }
    prof.render_ms = renders ? 1000.0 * render_s / double(renders) : 0.0;
    return prof;
}

}  // namespace mimicry::opt
