#include "features/Features.hpp"

#include <algorithm>
#include <cmath>

namespace mimicry::features {

namespace {

constexpr double kPi = 3.141592653589793;
constexpr std::size_t kMaxNoteWindow = std::size_t{1} << 18;

std::size_t round_size(double v) { return static_cast<std::size_t>(std::max(1.0, std::round(v))); }

struct BinRange {
    std::size_t lo = 1, hi = 0;
    [[nodiscard]] bool empty() const { return lo > hi; }
};

// Bin ranges of the harmonic bands and the gaps between them for an FFT of `nfft` points.
void harmonic_ranges(double f0, int K, int rate, std::size_t nfft, std::vector<BinRange>& harm,
                     std::vector<BinRange>& gap) {
    const double d = double(rate) / double(nfft);
    const std::size_t nb = nfft / 2 + 1;
    harm.assign(std::size_t(K), {});
    gap.assign(std::size_t(K), {});
    for (int k = 1; k <= K; ++k) {
        const double lo = (k * f0 - 0.25 * f0) / d, hi = (k * f0 + 0.25 * f0) / d;
        long l = long(std::ceil(lo)), h = long(std::floor(hi));
        if (h < l) l = h = long(std::lround(k * f0 / d));
        harm[std::size_t(k - 1)] = {std::size_t(std::max(1L, l)), std::size_t(std::min<long>(h, long(nb) - 1))};
    }
    for (int k = 1; k <= K; ++k) {
        const std::size_t lo = harm[std::size_t(k - 1)].hi + 1;
        std::size_t hi;
        if (k < K)
            hi = harm[std::size_t(k)].lo - 1;
        else
            hi = std::size_t(std::min<double>(std::floor(((k + 1) * f0 - 0.25 * f0) / d), double(nb) - 1));
        gap[std::size_t(k - 1)] = {lo, hi};
    }
}

float range_db(const float* p, const BinRange& r) {
    if (r.empty()) return kFloorDb;
    double s = 0;
    for (std::size_t k = r.lo; k <= r.hi; ++k) s += p[k];
    return power_to_db(s);
}

}  // namespace

std::vector<float> rms_envelope_db(const std::vector<float>& x, std::size_t window, std::size_t hop) {
    window = std::max<std::size_t>(window, 1);
    hop = std::max<std::size_t>(hop, 1);
    std::vector<double> cum(x.size() + 1, 0.0);
    for (std::size_t i = 0; i < x.size(); ++i) cum[i + 1] = cum[i] + double(x[i]) * x[i];
    const std::size_t frames = x.size() <= window ? 1 : (x.size() - window) / hop + 1;
    std::vector<float> env(frames);
    for (std::size_t f = 0; f < frames; ++f) {
        const std::size_t a = std::min(f * hop, x.size()), b = std::min(a + window, x.size());
        env[f] = power_to_db((cum[b] - cum[a]) / double(window));
    }
    return env;
}

long onset_frame(const std::vector<float>& env) {
    if (env.empty()) return -1;
    const float peak = *std::max_element(env.begin(), env.end());
    if (peak <= kFloorDb) return -1;
    std::vector<float> sorted(env);
    std::nth_element(sorted.begin(), sorted.begin() + long(sorted.size() / 20), sorted.end());
    const float floor_db = sorted[sorted.size() / 20];
    const float thr = std::min(std::max(floor_db + 12.0f, peak - 50.0f), peak - 6.0f);
    for (std::size_t i = 0; i < env.size(); ++i)
        if (env[i] >= thr) return long(i);
    return -1;
}

struct FeatureExtractor::Impl {
    explicit Impl(const FeatureConfig& c, int K) : cfg(c), K(K) {
        for (std::size_t s = 0; s < kMsstftSizes.size(); ++s) msstft.emplace_back(kMsstftSizes[s], kMsstftSizes[s]);
        band_n = std::min<std::size_t>(next_pow2(round_size(0.023 * c.sample_rate)), 8192);
        bands_stft = std::make_unique<PowerStft>(band_n, band_n);
        // 1/3-octave bands (ISO centres 1 kHz * 2^(i/3)); bands without a bin are dropped.
        const double d = double(c.sample_rate) / double(band_n), nyq = 0.5 * c.sample_rate;
        for (int i = -17; i <= 13; ++i) {
            const double centre = 1000.0 * std::pow(2.0, i / 3.0);
            const double lo = centre / std::pow(2.0, 1.0 / 6.0), hi = centre * std::pow(2.0, 1.0 / 6.0);
            if (hi > nyq) break;
            const long l = long(std::ceil(lo / d)), h = long(std::ceil(hi / d)) - 1;
            if (h >= l && l >= 1) band_ranges.push_back({std::size_t(l), std::size_t(h)});
        }
        if (K > 0) {
            harmt_win = round_size(0.04 * c.sample_rate);
            harmt_fft = next_pow2(harmt_win * 2);
            harmt_stft = std::make_unique<PowerStft>(harmt_win, harmt_fft);
            harmonic_ranges(c.f0, K, c.sample_rate, harmt_fft, harmt_harm, harmt_gap);
        }
        build_mel();
    }

    void build_mel() {
        const std::size_t nb = 2048 / 2 + 1;
        const double nyq = 0.5 * cfg.sample_rate, top = std::min(nyq, 16000.0), bottom = 20.0;
        auto mel = [](double f) { return 2595.0 * std::log10(1.0 + f / 700.0); };
        auto inv = [](double m) { return 700.0 * (std::pow(10.0, m / 2595.0) - 1.0); };
        const int nmel = 26;
        std::vector<double> edges(nmel + 2);
        for (int i = 0; i < nmel + 2; ++i) edges[std::size_t(i)] = inv(mel(bottom) + (mel(top) - mel(bottom)) * i / (nmel + 1));
        mel_w.assign(std::size_t(nmel) * nb, 0.0f);
        mel_lo.assign(std::size_t(nmel), nb);
        mel_hi.assign(std::size_t(nmel), 0);
        for (int m = 0; m < nmel; ++m)
            for (std::size_t k = 0; k < nb; ++k) {
                const double f = double(k) * cfg.sample_rate / 2048.0;
                const double a = edges[std::size_t(m)], b = edges[std::size_t(m + 1)], c = edges[std::size_t(m + 2)];
                double w = 0;
                if (f > a && f <= b) w = (f - a) / (b - a);
                else if (f > b && f < c) w = (c - f) / (c - b);
                mel_w[std::size_t(m) * nb + k] = float(w);
                if (w > 0) {
                    mel_lo[std::size_t(m)] = std::min(mel_lo[std::size_t(m)], k);
                    mel_hi[std::size_t(m)] = std::max(mel_hi[std::size_t(m)], k + 1);
                }
            }
        nmel_ = nmel;
        dct.assign(14 * std::size_t(nmel), 0.0);
        for (int c = 1; c <= 13; ++c)
            for (int j = 0; j < nmel; ++j) dct[std::size_t(c * nmel + j)] = std::cos(kPi * c * (j + 0.5) / nmel);
    }

    // `pw`: the 2048-point, hop-512 power STFT (msstft's middle size, the same window and hop), nf frames.
    std::vector<float> shape_features(const std::vector<float>& pw, std::size_t nf) {
        std::vector<float> out(kShapeDims, 0.0f);
        const std::size_t nb = 2048 / 2 + 1;
        std::vector<double> total(nf, 0.0);
        double max_total = 0;
        for (std::size_t f = 0; f < nf; ++f) {
            for (std::size_t k = 1; k < nb; ++k) total[f] += pw[f * nb + k];
            max_total = std::max(max_total, total[f]);
        }
        if (max_total <= 1e-12) return out;
        std::vector<double> acc(kShapeDims, 0.0), prev_m;
        std::size_t active = 0, flux_pairs = 0;
        std::vector<double> mel_db(static_cast<std::size_t>(nmel_));
        for (std::size_t f = 0; f < nf; ++f) {
            if (total[f] < max_total * 1e-6) {
                prev_m.clear();
                continue;
            }
            const float* p = &pw[f * nb];
            double csum = 0, log_sum = 0, cum = 0, roll = 0;
            for (std::size_t k = 1; k < nb; ++k) {
                const double fr = double(k) * cfg.sample_rate / 2048.0;
                csum += fr * p[k];
                log_sum += std::log(double(p[k]) + 1e-12);
            }
            const double centroid = csum / total[f];
            const double flat = 10.0 * std::log10(std::exp(log_sum / double(nb - 1)) / (total[f] / double(nb - 1)) + 1e-12);
            for (std::size_t k = 1; k < nb; ++k) {
                cum += p[k];
                if (cum >= 0.85 * total[f]) {
                    roll = double(k) * cfg.sample_rate / 2048.0;
                    break;
                }
            }
            std::vector<double> m(nb);
            double norm = 0;
            for (std::size_t k = 0; k < nb; ++k) {
                m[k] = std::sqrt(double(p[k]));
                norm += m[k] * m[k];
            }
            norm = std::sqrt(norm) + 1e-12;
            for (auto& v : m) v /= norm;
            if (!prev_m.empty()) {
                double flux = 0;
                for (std::size_t k = 0; k < nb; ++k) flux += std::max(0.0, m[k] - prev_m[k]);
                acc[3] += flux;
                ++flux_pairs;
            }
            prev_m = std::move(m);
            for (int j = 0; j < nmel_; ++j) {
                double e = 0;
                for (std::size_t k = mel_lo[std::size_t(j)]; k < mel_hi[std::size_t(j)]; ++k) e += mel_w[std::size_t(j) * nb + k] * p[k];
                mel_db[std::size_t(j)] = 10.0 * std::log10(e + 1e-12);
            }
            for (int c = 1; c <= 13; ++c) {
                double s = 0;
                for (int j = 0; j < nmel_; ++j) s += mel_db[std::size_t(j)] * dct[std::size_t(c * nmel_ + j)];
                acc[std::size_t(3 + c)] += s;
            }
            acc[0] += std::log2(std::max(centroid, 1.0));
            acc[1] += flat;
            acc[2] += std::log2(std::max(roll, 1.0));
            ++active;
        }
        if (active == 0) return out;
        for (std::size_t i : {std::size_t{0}, std::size_t{1}, std::size_t{2}}) out[i] = float(acc[i] / double(active));
        out[3] = flux_pairs ? float(acc[3] / double(flux_pairs)) : 0.0f;
        for (std::size_t i = 4; i < kShapeDims; ++i) out[i] = float(acc[i] / double(active));
        return out;
    }

    FeatureConfig cfg;
    int K;
    std::vector<PowerStft> msstft;
    std::size_t band_n = 1024;
    std::unique_ptr<PowerStft> bands_stft, harmt_stft, note_stft;
    std::vector<BinRange> band_ranges;
    std::size_t harmt_win = 0, harmt_fft = 0, note_window = 0;
    std::vector<BinRange> harmt_harm, harmt_gap, note_harm, note_gap;
    std::vector<float> mel_w;
    std::vector<std::size_t> mel_lo, mel_hi;
    std::vector<double> dct;
    int nmel_ = 26;
};

FeatureExtractor::FeatureExtractor(const FeatureConfig& config) : config_(config) {
    if (config.f0 > 0) {
        const double nyq = 0.5 * config.sample_rate;
        harmonics_ = std::clamp(int(std::floor(0.95 * nyq / config.f0 - 0.25)), 0, config.max_harmonics);
    }
    impl_ = std::make_unique<Impl>(config, harmonics_);
}

FeatureExtractor::~FeatureExtractor() = default;

FeatureSet FeatureExtractor::extract(const std::vector<float>& mono, std::size_t length) {
    auto& im = *impl_;
    std::vector<float> x(mono);
    x.resize(length, 0.0f);
    FeatureSet fs;
    fs.length = length;
    fs.harmonics = harmonics_;
    const int rate = config_.sample_rate;

    static_assert(kMsstftSizes[1] == 2048, "shape reuses the 2048-point msstft");
    std::vector<float> shape_power;
    for (std::size_t s = 0; s < kMsstftSizes.size(); ++s) {
        const std::size_t n = kMsstftSizes[s], hop = n / 4;
        Grid& g = fs.msstft[s];
        g.cols = n / 2 + 1;
        g.frames = im.msstft[s].frames(length, hop);
        g.v = im.msstft[s].power(x.data(), length, hop);
        if (n == 2048) shape_power = g.v;  // `shape` reads the same STFT in power, not dB (no second transform)
        for (auto& p : g.v) p = fast_power_db(p);
    }

    const std::size_t env_win = config_.f0 > 0 ? round_size(2.0 * rate / config_.f0) : round_size(0.005 * rate);
    fs.env.v = rms_envelope_db(x, std::max<std::size_t>(env_win, 16), round_size(0.001 * rate));
    fs.env.frames = fs.env.v.size();
    fs.env.cols = 1;

    {
        const std::size_t hop = im.band_n / 2, nb = im.bands_stft->bins();
        const auto pw = im.bands_stft->power(x.data(), length, hop);
        fs.bands.frames = im.bands_stft->frames(length, hop);
        fs.bands.cols = im.band_ranges.size();
        fs.bands.v.resize(fs.bands.frames * fs.bands.cols);
        for (std::size_t f = 0; f < fs.bands.frames; ++f)
            for (std::size_t b = 0; b < fs.bands.cols; ++b) {
                const auto& r = im.band_ranges[b];
                fs.bands.v[f * fs.bands.cols + b] = range_db(&pw[f * nb], {r.lo, std::min(r.hi, nb - 1)});
            }
    }

    if (harmonics_ > 0) {
        const std::size_t K = std::size_t(harmonics_);
        const std::size_t window = std::min(std::max<std::size_t>(length, 64), kMaxNoteWindow);
        if (!im.note_stft || im.note_window != window) {
            im.note_stft = std::make_unique<PowerStft>(window, std::max<std::size_t>(4096, next_pow2(window)));
            im.note_window = window;
            harmonic_ranges(config_.f0, harmonics_, rate, im.note_stft->fft_size(), im.note_harm, im.note_gap);
        }
        std::vector<float> spec(im.note_stft->bins());
        im.note_stft->frame_power(x.data(), length, 0, spec.data());
        fs.harm.resize(K);
        fs.inter.resize(K);
        for (std::size_t k = 0; k < K; ++k) {
            fs.harm[k] = range_db(spec.data(), im.note_harm[k]);
            fs.inter[k] = range_db(spec.data(), im.note_gap[k]);
        }
        const std::size_t hop = round_size(0.01 * rate), nb = im.harmt_stft->bins();
        const auto pw = im.harmt_stft->power(x.data(), length, hop);
        fs.harmt.frames = im.harmt_stft->frames(length, hop);
        fs.harmt.cols = K;
        fs.harmt.v.resize(fs.harmt.frames * K);
        for (std::size_t f = 0; f < fs.harmt.frames; ++f)
            for (std::size_t k = 0; k < K; ++k) fs.harmt.v[f * K + k] = range_db(&pw[f * nb], im.harmt_harm[k]);
    }

    fs.shape = im.shape_features(shape_power, fs.msstft[1].frames);
    return fs;
}

}  // namespace mimicry::features
