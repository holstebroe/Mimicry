#include "features/Preprocess.hpp"

#include <algorithm>
#include <cmath>

#include "features/Features.hpp"

namespace mimicry::features {

std::vector<float> mix_to_mono(const std::vector<float>& x, int channels) {
    if (channels <= 1) return x;
    const std::size_t ch = std::size_t(channels), frames = x.size() / ch;
    std::vector<float> mono(frames);
    for (std::size_t i = 0; i < frames; ++i) {
        double s = 0;
        for (std::size_t c = 0; c < ch; ++c) s += x[i * ch + c];
        mono[i] = float(s / double(ch));
    }
    return mono;
}

std::vector<float> resample(const std::vector<float>& x, int from, int to) {
    if (from == to || x.empty() || from <= 0 || to <= 0) return x;
    const double ratio = double(to) / double(from);
    const double cutoff = std::min(1.0, ratio) * 0.97;  // fraction of the lower Nyquist kept
    const int zeros = 32;
    const double half = zeros / cutoff;  // kernel half-width in input samples
    const std::size_t out_n = std::size_t(std::llround(double(x.size()) * ratio));
    std::vector<float> y(out_n);
    constexpr double pi = 3.141592653589793;
    for (std::size_t i = 0; i < out_n; ++i) {
        const double pos = double(i) / ratio;
        const long lo = long(std::ceil(pos - half)), hi = long(std::floor(pos + half));
        double acc = 0;
        for (long j = lo; j <= hi; ++j) {
            if (j < 0 || j >= long(x.size())) continue;
            const double t = (double(j) - pos) * cutoff;
            const double sinc = t == 0 ? 1.0 : std::sin(pi * t) / (pi * t);
            const double u = (double(j) - pos) / half;  // -1..1
            const double w = 0.42 + 0.5 * std::cos(pi * u) + 0.08 * std::cos(2 * pi * u);
            acc += x[std::size_t(j)] * sinc * w * cutoff;
        }
        y[i] = float(acc);
    }
    return y;
}

void remove_dc(std::vector<float>& x, int rate) {
    const auto env = rms_envelope_db(x, std::size_t(std::max(1, rate / 200)), std::size_t(std::max(1, rate / 1000)));
    const long onset = onset_frame(env);
    if (onset < 10) return;
    const std::size_t n = std::min<std::size_t>(x.size(), std::size_t(onset) * std::size_t(std::max(1, rate / 1000)) - std::size_t(rate / 200));
    if (n < 16) return;
    double mean = 0;
    for (std::size_t i = 0; i < n; ++i) mean += x[i];
    mean /= double(n);
    for (auto& v : x) v = float(v - mean);
}

}  // namespace mimicry::features
