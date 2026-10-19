#include "features/Loss.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>

#include "features/Pitch.hpp"
#include "features/Preprocess.hpp"

namespace mimicry::features {

namespace {

constexpr float kAboveFloorDb = 3.0f;  // cells counted for the gain solve [D]

float percentile(std::vector<float> v, double q) {
    if (v.empty()) return kFloorDb;
    const std::size_t i = std::min(v.size() - 1, std::size_t(q * double(v.size())));
    std::nth_element(v.begin(), v.begin() + long(i), v.end());
    return v[i];
}

float grid_peak(const Grid& g) {
    return g.v.empty() ? kFloorDb : *std::max_element(g.v.begin(), g.v.end());
}

double power_mean_db(float a, float b) {
    return 10.0 * std::log10(0.5 * (std::pow(10.0, a / 10.0) + std::pow(10.0, b / 10.0)) + 1e-12);
}

SampleKind classify(const TargetAnalysis& a) {
    if (a.confidence >= 0.5 && a.f0 > 0) return SampleKind::Note;
    if (a.onsets >= 3 && a.duration_ms >= 2000) return SampleKind::Loop;
    if (a.duration_ms < 1500 && a.attack_ms < 20) return SampleKind::Hit;
    return SampleKind::Texture;
}

}  // namespace

std::string_view to_string(SampleKind k) noexcept {
    switch (k) {
        case SampleKind::Note: return "note";
        case SampleKind::Hit: return "hit";
        case SampleKind::Texture: return "texture";
        case SampleKind::Loop: return "loop";
    }
    return "texture";
}

std::optional<SampleKind> parse_kind(std::string_view t) {
    for (auto k : {SampleKind::Note, SampleKind::Hit, SampleKind::Texture, SampleKind::Loop})
        if (t == to_string(k)) return k;
    return std::nullopt;
}

std::string_view to_string(Feature f) noexcept {
    static constexpr std::string_view names[kFeatureCount] = {"msstft", "harm", "inter", "harmt", "env", "bands", "shape"};
    return names[std::size_t(f)];
}

Weights Weights::preset(SampleKind kind) {
    Weights x;
    //                 msstft harm inter harmt env  bands shape
    switch (kind) {
        case SampleKind::Note: x.w = {1, 1, 0.5, 0.5, 1, 0.5, 0}; break;
        case SampleKind::Hit: x.w = {1, 0, 0, 0, 1.5, 1, 0.25}; break;
        case SampleKind::Texture: x.w = {1, 0, 0, 0, 0.5, 1, 1}; break;
        case SampleKind::Loop: x.w = {1, 0, 0, 0, 1, 1, 0.5}; break;
    }
    return x;
}

bool Weights::set(std::string_view name, double value) {
    for (std::size_t i = 0; i < kFeatureCount; ++i)
        if (to_string(Feature(i)) == name) {
            w[i] = value;
            return true;
        }
    return false;
}

expected<Target, std::string> Target::from_wav(const WavData& wav, const AnalyzeOptions& opt) {
    if (wav.channels <= 0 || wav.sample_rate <= 0 || wav.samples.empty()) return unexpected(std::string("empty WAV"));
    return from_mono(mix_to_mono(wav.samples, wav.channels), wav.sample_rate, opt);
}

expected<Target, std::string> Target::from_mono(std::vector<float> mono, int rate, const AnalyzeOptions& opt) {
    if (rate <= 0 || opt.sample_rate <= 0) return unexpected(std::string("bad sample rate"));
    Target t;
    t.mono_ = resample(mono, rate, opt.sample_rate);
    remove_dc(t.mono_, opt.sample_rate);
    const int sr = opt.sample_rate;
    if (t.mono_.size() < 256) return unexpected(std::string("target is too short"));

    TargetAnalysis& a = t.analysis_;
    const auto pitch = estimate_f0(t.mono_, sr);
    a.f0 = pitch.f0;
    a.confidence = pitch.confidence;
    if (opt.key) {
        // The key is given; the cents are whatever the measurement says is left over.
        const double target = hz_from_note(*opt.key);
        a.key = *opt.key;
        a.cents = pitch.f0 > 0 ? 1200.0 * std::log2(pitch.f0 / target) : 0.0;
        a.cents = std::clamp(a.cents, -50.0, 50.0);
        if (pitch.f0 <= 0) a.f0 = target, a.confidence = std::max(a.confidence, 0.5);
    } else if (pitch.f0 > 0) {
        const auto np = note_from_hz(pitch.f0);
        a.key = np.key;
        a.cents = np.cents;
    }

    // Envelope analysis on a 5 ms window (the 2-period one needs the f0 and is part of the features).
    const std::size_t hop = std::max<std::size_t>(1, std::size_t(sr / 1000));
    const auto env = rms_envelope_db(t.mono_, std::max<std::size_t>(16, std::size_t(sr / 200)), hop);
    const long on = onset_frame(env);
    const float peak = env.empty() ? kFloorDb : *std::max_element(env.begin(), env.end());
    a.onset_ms = on < 0 ? 0.0 : double(on);
    a.floor_db = percentile(env, 0.05);
    long last = on, c30 = -1, c3 = -1, peak_i = 0;
    for (std::size_t i = 0; i < env.size(); ++i) {
        if (env[i] >= peak - 40.0f) last = long(i);
        if (env[i] >= peak - 0.01f && peak_i == 0) peak_i = long(i);
    }
    for (long i = std::max(on, 0L); i <= peak_i; ++i) {
        if (c30 < 0 && env[std::size_t(i)] >= peak - 30.0f) c30 = i;
        if (c3 < 0 && env[std::size_t(i)] >= peak - 3.0f) c3 = i;
    }
    a.duration_ms = on < 0 ? 0.0 : double(last - on + 1);
    a.attack_ms = c30 >= 0 && c3 >= 0 ? double(c3 - c30) : 0.0;
    // Separate onsets: a rise of 9 dB within 20 ms, at least 100 ms after the previous one.
    long prev = -1000;
    for (std::size_t i = 20; i < env.size(); ++i)
        if (env[i] - env[i - 20] > 9.0f && env[i] > peak - 35.0f && long(i) - prev > 100) {
            ++a.onsets;
            prev = long(i);
        }
    a.kind = opt.kind ? *opt.kind : classify(a);

    t.config_.sample_rate = sr;
    t.config_.f0 = a.f0 > 0 && (a.kind == SampleKind::Note || opt.key) ? a.f0 : 0.0;
    FeatureExtractor ex(t.config_);
    t.features_ = ex.extract(t.mono_, t.mono_.size());

    // Noise floors (spec 03 section 2): quietest content of the target itself.
    const FeatureSet& f = t.features_;
    Floors& fl = t.floors_;
    for (std::size_t s = 0; s < 3; ++s) {
        const Grid& g = f.msstft[s];
        fl.msstft[s].resize(g.cols);
        std::vector<float> col(g.frames);
        for (std::size_t c = 0; c < g.cols; ++c) {
            for (std::size_t r = 0; r < g.frames; ++r) col[r] = g.v[r * g.cols + c];
            fl.msstft[s][c] = percentile(col, 0.10);  // 10th percentile of each bin [D]
        }
    }
    fl.bands.resize(f.bands.cols);
    for (std::size_t b = 0; b < f.bands.cols; ++b) {
        float m = f.bands.frames ? 1e9f : kFloorDb;
        for (std::size_t r = 0; r < f.bands.frames; ++r) m = std::min(m, f.bands.v[r * f.bands.cols + b]);
        fl.bands[b] = m;  // quietest frame of each band [S]
    }
    fl.harm.resize(f.harm.size());
    for (std::size_t k = 0; k < f.harm.size(); ++k) {
        const double below = k > 0 ? double(f.inter[k - 1]) : double(f.inter[k]);
        fl.harm[k] = float(power_mean_db(float(below), f.inter[k]));  // inter-harmonic level beside each harmonic
    }
    return t;
}

Scorer::Scorer(const Target& target, Weights weights, ScorerOptions options)
    : target_(target), weights_(weights), options_(options), extractor_(target.config()) {
    const FeatureSet& f = target.features();
    const float dr = float(options_.dynamic_range_db);
    for (std::size_t s = 0; s < 3; ++s) {
        const float lim = grid_peak(f.msstft[s]) - dr;
        lo_msstft_[s] = target.floors().msstft[s];
        for (auto& v : lo_msstft_[s]) v = std::max(v, lim);
        t_msstft_[s] = f.msstft[s];
        for (std::size_t r = 0; r < t_msstft_[s].frames; ++r)
            for (std::size_t c = 0; c < t_msstft_[s].cols; ++c) {
                float& v = t_msstft_[s].v[r * t_msstft_[s].cols + c];
                v = std::max(v, lo_msstft_[s][c]);
            }
    }
    const float lim_b = grid_peak(f.bands) - dr;
    lo_bands_ = target.floors().bands;
    for (auto& v : lo_bands_) v = std::max(v, lim_b);
    t_bands_ = f.bands;
    for (std::size_t r = 0; r < t_bands_.frames; ++r)
        for (std::size_t c = 0; c < t_bands_.cols; ++c) {
            float& v = t_bands_.v[r * t_bands_.cols + c];
            v = std::max(v, lo_bands_[c]);
        }
    lo_env_ = grid_peak(f.env) - dr;
    t_env_ = f.env;
    for (auto& v : t_env_.v) v = std::max(v, lo_env_);

    if (f.harmonics > 0) {
        const float hpeak = *std::max_element(f.harm.begin(), f.harm.end());
        lo_harm_ = target.floors().harm;
        for (auto& v : lo_harm_) v = std::max(v, hpeak - dr);
        t_harm_ = f.harm;
        for (std::size_t k = 0; k < t_harm_.size(); ++k) t_harm_[k] = std::max(t_harm_[k], lo_harm_[k]);
        lo_inter_ = hpeak - dr;
        clamped_inter_ = f.inter;
        for (auto& v : clamped_inter_) v = std::max(v, lo_inter_);
        lo_harmt_ = grid_peak(f.harmt) - dr;
        t_harmt_ = f.harmt;
        for (auto& v : t_harmt_.v) v = std::max(v, lo_harmt_);
        harm_weight_.resize(std::size_t(f.harmonics));
        for (int k = 1; k <= f.harmonics; ++k) harm_weight_[std::size_t(k - 1)] = std::pow(double(k), -0.5);
    } else {
        weights_.w[std::size_t(Feature::Harm)] = weights_.w[std::size_t(Feature::Inter)] =
            weights_.w[std::size_t(Feature::Harmt)] = 0;
    }
}

long Scorer::estimate_offset(const std::vector<float>& cand) const {
    const int sr = target_.config().sample_rate;
    const std::size_t hop = std::max<std::size_t>(1, std::size_t(sr / 1000));
    const long c = onset_frame(rms_envelope_db(cand, std::max<std::size_t>(16, std::size_t(sr / 200)), hop));
    const long t = onset_frame(rms_envelope_db(target_.mono(), std::max<std::size_t>(16, std::size_t(sr / 200)), hop));
    if (c < 0 || t < 0) return 0;
    return (t - c) * long(hop);
}

Score Scorer::score(const std::vector<float>& candidate) {
    const std::size_t n = target_.length();
    std::vector<float> x(n, 0.0f);
    const long off = options_.offset_samples;
    for (std::size_t i = 0; i < n; ++i) {
        const long src = long(i) - off;
        if (src >= 0 && src < long(candidate.size())) x[i] = candidate[std::size_t(src)];
    }
    const FeatureSet c = extractor_.extract(x, n);
    const FeatureSet& t = target_.features();
    Score s;

    // Level: one global gain, the median dB offset over the cells above the floor in both (L1 optimum).
    if (options_.solve_gain) {
        std::vector<float> diffs;
        const Grid& tg = t.msstft[1];
        const Grid& cg = c.msstft[1];
        for (std::size_t r = 0; r < tg.frames; ++r)
            for (std::size_t k = 0; k < tg.cols; ++k) {
                const float tv = tg.v[r * tg.cols + k], cv = cg.v[r * cg.cols + k];
                if (tv >= lo_msstft_[1][k] + kAboveFloorDb && cv >= lo_msstft_[1][k] + kAboveFloorDb) diffs.push_back(tv - cv);
            }
        if (!diffs.empty()) {
            const std::size_t m = diffs.size() / 2;
            std::nth_element(diffs.begin(), diffs.begin() + long(m), diffs.end());
            s.gain_db = diffs[m];
        }
    }
    const float g = float(s.gain_db);
    // Digital silence stays silent whatever the gain.
    auto lvl = [&](float v, float lo) { return v <= kFloorDb ? lo : std::max(v + g, lo); };

    double msstft = 0;
    for (std::size_t z = 0; z < 3; ++z) {
        double acc = 0;
        const Grid& tg = t_msstft_[z];
        const Grid& cg = c.msstft[z];
        for (std::size_t r = 0; r < tg.frames; ++r)
            for (std::size_t k = 0; k < tg.cols; ++k)
                acc += std::fabs(tg.v[r * tg.cols + k] - lvl(cg.v[r * cg.cols + k], lo_msstft_[z][k]));
        msstft += acc / double(std::max<std::size_t>(1, tg.v.size()));
    }
    s.loss[std::size_t(Feature::Msstft)] = msstft / 3.0;

    {
        double acc = 0;
        for (std::size_t r = 0; r < t_bands_.frames; ++r)
            for (std::size_t b = 0; b < t_bands_.cols; ++b)
                acc += std::fabs(t_bands_.v[r * t_bands_.cols + b] - lvl(c.bands.v[r * c.bands.cols + b], lo_bands_[b]));
        s.loss[std::size_t(Feature::Bands)] = acc / double(std::max<std::size_t>(1, t_bands_.v.size()));
    }
    {
        double acc = 0;
        const std::size_t m = std::min(t_env_.v.size(), c.env.v.size());
        for (std::size_t i = 0; i < m; ++i) acc += std::fabs(t_env_.v[i] - lvl(c.env.v[i], lo_env_));
        s.loss[std::size_t(Feature::Env)] = m ? acc / double(m) : 0.0;
    }
    if (t.harmonics > 0) {
        const std::size_t K = std::size_t(t.harmonics);
        double acc = 0, wsum = 0;
        for (std::size_t k = 0; k < K; ++k) {
            acc += harm_weight_[k] * std::fabs(t_harm_[k] - lvl(c.harm[k], lo_harm_[k]));
            wsum += harm_weight_[k];
        }
        s.loss[std::size_t(Feature::Harm)] = acc / wsum;
        // Only content the candidate adds on top of the target's own noise counts.
        acc = 0;
        for (std::size_t k = 0; k < K; ++k) acc += std::max(0.0f, lvl(c.inter[k], lo_inter_) - clamped_inter_[k]);
        s.loss[std::size_t(Feature::Inter)] = acc / double(K);
        acc = 0;
        wsum = 0;
        for (std::size_t r = 0; r < t_harmt_.frames; ++r)
            for (std::size_t k = 0; k < K; ++k) {
                acc += harm_weight_[k] * std::fabs(t_harmt_.v[r * K + k] - lvl(c.harmt.v[r * K + k], lo_harmt_));
                wsum += harm_weight_[k];
            }
        s.loss[std::size_t(Feature::Harmt)] = wsum > 0 ? acc / wsum : 0.0;
    }
    {
        // Shape [D]: octave differences x 6 and the flux x 10 so every component is in dB-like units.
        const auto& a = t.shape;
        const auto& b = c.shape;
        double mfcc = 0;
        for (std::size_t i = 4; i < kShapeDims; ++i) mfcc += double(a[i] - b[i]) * double(a[i] - b[i]);
        mfcc = std::sqrt(mfcc / 13.0);
        const double parts[5] = {6.0 * std::fabs(a[0] - b[0]), std::fabs(a[1] - b[1]), 6.0 * std::fabs(a[2] - b[2]),
                                 10.0 * std::fabs(a[3] - b[3]), mfcc};
        s.loss[std::size_t(Feature::Shape)] = std::accumulate(parts, parts + 5, 0.0) / 5.0;
    }

    double num = 0, den = 0;
    for (std::size_t i = 0; i < kFeatureCount; ++i)
        if (weights_.w[i] > 0) {
            num += weights_.w[i] * s.loss[i];
            den += weights_.w[i];
        }
    s.cost = den > 0 ? num / den : 0.0;
    return s;
}

RepeatPolicy repeat_policy(double floor_db) {
    RepeatPolicy p;
    p.floor_db = floor_db;
    p.renders = floor_db < 0.5 ? 1 : std::clamp(int(std::ceil(floor_db / 0.5)) + 1, 2, 4);  // [D]
    p.success_db = 1.5 * floor_db;
    return p;
}

double repeatability_floor_db(const std::vector<float>& a, const std::vector<float>& b, int rate, const Weights& w) {
    AnalyzeOptions opt;
    opt.sample_rate = rate;
    auto t = Target::from_mono(a, rate, opt);
    if (!t) return 0.0;
    Scorer scorer(*t, w);
    return scorer.score(b).cost;
}

}  // namespace mimicry::features
