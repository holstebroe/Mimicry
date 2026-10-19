#include "opt/Embedding.hpp"

#include <algorithm>
#include <cmath>

#include "features/Features.hpp"
#include "features/Spectral.hpp"

namespace mimicry::opt {

namespace {
constexpr std::size_t kMel = 32;
constexpr std::size_t kN = 2048, kHop = 512;
}  // namespace

struct Embedder::Impl {
    explicit Impl(int rate) : stft(kN, kN) {
        const std::size_t nb = stft.bins();
        auto mel = [](double f) { return 2595.0 * std::log10(1.0 + f / 700.0); };
        auto inv = [](double m) { return 700.0 * (std::pow(10.0, m / 2595.0) - 1.0); };
        const double bottom = 30.0, top = std::min(16000.0, 0.5 * rate);
        std::vector<double> edges(kMel + 2);
        for (std::size_t i = 0; i < kMel + 2; ++i)
            edges[i] = inv(mel(bottom) + (mel(top) - mel(bottom)) * double(i) / double(kMel + 1));
        w.assign(kMel * nb, 0.0f);
        const double d = double(rate) / double(kN);
        for (std::size_t m = 0; m < kMel; ++m)
            for (std::size_t k = 1; k < nb; ++k) {
                const double f = double(k) * d;
                double v = 0;
                if (f > edges[m] && f <= edges[m + 1]) v = (f - edges[m]) / (edges[m + 1] - edges[m]);
                else if (f > edges[m + 1] && f < edges[m + 2]) v = (edges[m + 2] - f) / (edges[m + 2] - edges[m + 1]);
                w[m * nb + k] = float(v);
            }
        // A band narrower than one bin gets the nearest bin, so no band is empty at low rates.
        for (std::size_t m = 0; m < kMel; ++m) {
            bool any = false;
            for (std::size_t k = 0; k < nb; ++k) any = any || w[m * nb + k] > 0;
            if (!any) w[m * nb + std::min(nb - 1, std::size_t(std::lround(edges[m + 1] / d)))] = 1.0f;
        }
    }
    features::PowerStft stft;
    std::vector<float> w;
};

Embedder::Embedder(int sample_rate) : rate_(sample_rate), impl_(std::make_unique<Impl>(sample_rate)) {}
Embedder::~Embedder() = default;

Embedding Embedder::embed(const std::vector<float>& x) {
    Embedding e(kEmbeddingDims, 0.0f);
    const std::size_t nb = impl_->stft.bins();
    const std::size_t nf = impl_->stft.frames(x.size(), kHop);
    const auto pw = impl_->stft.power(x.data(), x.size(), kHop);
    const double d = double(rate_) / double(kN);

    std::vector<double> energy(nf, 0.0);
    for (std::size_t f = 0; f < nf; ++f)
        for (std::size_t k = 1; k < nb; ++k) energy[f] += pw[f * nb + k];
    const double emax = nf ? *std::max_element(energy.begin(), energy.end()) : 0.0;
    if (!(emax > 1e-14)) {
        for (std::size_t i = 0; i < kEmbeddingEnvelopeMs.size(); ++i) e[kEmbeddingEnvelope + i] = -80.0f;
        e[kEmbeddingLevel] = features::kFloorDb;
        return e;
    }

    std::vector<double> sum(kMel, 0.0), sum2(kMel, 0.0), mel(kMel);
    double cs = 0, cs2 = 0, fs = 0, fs2 = 0, level = 0;
    std::size_t active = 0;
    for (std::size_t f = 0; f < nf; ++f) {
        if (energy[f] < emax * 1e-6) continue;  // within 60 dB of the loudest frame
        const float* p = &pw[f * nb];
        // Floors relative to the frame (120 dB down), so a quieter copy of a sound embeds the same.
        const double floor = energy[f] * 1e-12;
        for (std::size_t m = 0; m < kMel; ++m) {
            double s = 0;
            const float* wm = &impl_->w[m * nb];
            for (std::size_t k = 1; k < nb; ++k) s += double(wm[k]) * p[k];
            mel[m] = 10.0 * std::log10(s + floor);
            sum[m] += mel[m];
            sum2[m] += mel[m] * mel[m];
        }
        double num = 0, logsum = 0;
        for (std::size_t k = 1; k < nb; ++k) {
            num += double(k) * d * p[k];
            logsum += std::log(double(p[k]) + floor);
        }
        const double centroid = 6.0 * std::log2(std::max(num / energy[f], 1.0));
        const double geo = std::exp(logsum / double(nb - 1));
        const double flat = 10.0 * std::log10(geo / (energy[f] / double(nb - 1)) + 1e-12);
        cs += centroid;
        cs2 += centroid * centroid;
        fs += flat;
        fs2 += flat * flat;
        level += energy[f];
        ++active;
    }
    const double n = double(active);
    double overall = 0;
    for (std::size_t m = 0; m < kMel; ++m) overall += sum[m] / n;
    overall /= double(kMel);
    for (std::size_t m = 0; m < kMel; ++m) {
        const double mean = sum[m] / n;
        e[m] = float(mean - overall);
        e[kMel + m] = float(std::sqrt(std::max(0.0, sum2[m] / n - mean * mean)));
    }
    auto stats = [n](double s, double s2, float* out) {
        const double mean = s / n;
        out[0] = float(mean);
        out[1] = float(std::sqrt(std::max(0.0, s2 / n - mean * mean)));
    };
    stats(cs, cs2, &e[kEmbeddingCentroid]);
    stats(fs, fs2, &e[kEmbeddingFlatness]);

    // Envelope (10 ms frames) relative to its peak, sampled after the onset.
    const std::size_t hop = std::max<std::size_t>(1, std::size_t(rate_ / 100));
    const auto env = features::rms_envelope_db(x, hop, hop);
    const float peak = *std::max_element(env.begin(), env.end());
    const long on = std::max(0L, features::onset_frame(env));
    for (std::size_t i = 0; i < kEmbeddingEnvelopeMs.size(); ++i) {
        const std::size_t f = std::size_t(on) + std::size_t(kEmbeddingEnvelopeMs[i] / 10.0);
        const float v = f < env.size() ? env[f] - peak : -80.0f;
        e[kEmbeddingEnvelope + i] = std::max(v, -80.0f);
    }
    e[kEmbeddingLevel] = float(10.0 * std::log10(level / n / double(kN) + 1e-12));
    return e;
}

double embedding_distance(const Embedding& a, const Embedding& b) {
    if (a.size() != kEmbeddingDims || b.size() != kEmbeddingDims) return 1e9;
    double s = 0;
    for (std::size_t i = 0; i < kEmbeddingDims; ++i) {
        if (i == kEmbeddingLevel) continue;
        const double v = double(a[i]) - double(b[i]);
        s += v * v;
    }
    return std::sqrt(s / double(kEmbeddingDims - 1));
}

}  // namespace mimicry::opt
