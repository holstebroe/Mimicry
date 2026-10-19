#include "features/Spectral.hpp"

#include <algorithm>
#include <cmath>

namespace mimicry::features {

float power_to_db(double power) noexcept {
    if (!(power > 1e-12)) return kFloorDb;
    return static_cast<float>(10.0 * std::log10(power));
}

std::size_t next_pow2(std::size_t n) noexcept {
    std::size_t p = 2;
    while (p < n) p <<= 1;
    return p;
}

PowerStft::PowerStft(std::size_t window, std::size_t fft_size) : fft_(fft_size), window_(window) {
    double sum = 0;
    for (std::size_t i = 0; i < window; ++i) {
        window_[i] = 0.5 - 0.5 * std::cos(2.0 * 3.141592653589793 * (static_cast<double>(i) + 0.5) /
                                          static_cast<double>(window));
        sum += window_[i];
    }
    norm_ = sum > 0 ? 1.0 / (sum * sum) : 1.0;
    buf_.resize(fft_size);
}

std::size_t PowerStft::frames(std::size_t length, std::size_t hop) const noexcept {
    const std::size_t w = window_.size();
    return length <= w ? 1 : (length - w) / hop + 1;
}

void PowerStft::load(const float* x, std::size_t length, std::size_t start,
                     std::vector<std::complex<double>>& buf, bool imaginary) const {
    const std::size_t w = window_.size();
    for (std::size_t i = 0; i < w; ++i) {
        const double v = start + i < length ? x[start + i] * window_[i] : 0.0;
        if (imaginary)
            buf[i] = {buf[i].real(), v};
        else
            buf[i] = {v, 0.0};
    }
}

std::vector<float> PowerStft::power(const float* x, std::size_t length, std::size_t hop) {
    const std::size_t n = fft_.size(), nb = bins(), nf = frames(length, hop);
    std::vector<float> out(nf * nb);
    for (std::size_t f = 0; f < nf; f += 2) {
        std::fill(buf_.begin(), buf_.end(), std::complex<double>{});
        load(x, length, f * hop, buf_, false);
        const bool pair = f + 1 < nf;
        if (pair) load(x, length, (f + 1) * hop, buf_, true);
        fft_.forward(buf_);
        float* a = &out[f * nb];
        float* b = pair ? &out[(f + 1) * nb] : nullptr;
        for (std::size_t k = 0; k < nb; ++k) {
            const auto z = buf_[k];
            const auto zc = std::conj(buf_[(n - k) % n]);
            const auto xa = 0.5 * (z + zc);
            a[k] = static_cast<float>(std::norm(xa) * norm_);
            if (b) {
                const auto xb = (z - zc) * std::complex<double>(0.0, -0.5);
                b[k] = static_cast<float>(std::norm(xb) * norm_);
            }
        }
    }
    return out;
}

void PowerStft::frame_power(const float* x, std::size_t length, std::size_t start, float* out) {
    std::fill(buf_.begin(), buf_.end(), std::complex<double>{});
    load(x, length, start, buf_, false);
    fft_.forward(buf_);
    for (std::size_t k = 0; k < bins(); ++k) out[k] = static_cast<float>(std::norm(buf_[k]) * norm_);
}

}  // namespace mimicry::features
