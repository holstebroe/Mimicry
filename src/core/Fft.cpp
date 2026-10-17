#include "core/Fft.hpp"

#include <cmath>
#include <stdexcept>

namespace mimicry {

Fft::Fft(std::size_t size) : n_(size) {
    if (!is_power_of_two(size)) throw std::invalid_argument("Fft: size must be a power of two");
    twiddle_.resize(n_ / 2);
    for (std::size_t k = 0; k < n_ / 2; ++k) {
        const double a = -2.0 * 3.141592653589793238462643 * static_cast<double>(k) / static_cast<double>(n_);
        twiddle_[k] = {std::cos(a), std::sin(a)};
    }
    rev_.resize(n_);
    std::size_t bits = 0;
    while ((std::size_t{1} << bits) < n_) ++bits;
    for (std::size_t i = 0; i < n_; ++i) {
        std::size_t r = 0;
        for (std::size_t b = 0; b < bits; ++b)
            if (i & (std::size_t{1} << b)) r |= std::size_t{1} << (bits - 1 - b);
        rev_[i] = r;
    }
}

void Fft::transform(std::vector<std::complex<double>>& d, bool inverse) const {
    if (d.size() != n_) throw std::invalid_argument("Fft: wrong buffer size");
    for (std::size_t i = 0; i < n_; ++i)
        if (i < rev_[i]) std::swap(d[i], d[rev_[i]]);
    for (std::size_t len = 2; len <= n_; len <<= 1) {
        const std::size_t half = len / 2, step = n_ / len;
        for (std::size_t i = 0; i < n_; i += len) {
            for (std::size_t j = 0; j < half; ++j) {
                auto w = twiddle_[j * step];
                if (inverse) w = std::conj(w);
                const auto u = d[i + j];
                const auto v = d[i + j + half] * w;
                d[i + j] = u + v;
                d[i + j + half] = u - v;
            }
        }
    }
    if (inverse) {
        const double s = 1.0 / static_cast<double>(n_);
        for (auto& x : d) x *= s;
    }
}

void Fft::forward(std::vector<std::complex<double>>& data) const { transform(data, false); }
void Fft::inverse(std::vector<std::complex<double>>& data) const { transform(data, true); }

std::vector<double> Fft::magnitude(const float* signal) const {
    std::vector<std::complex<double>> buf(n_);
    for (std::size_t i = 0; i < n_; ++i) buf[i] = signal[i];
    transform(buf, false);
    std::vector<double> mag(n_ / 2 + 1);
    for (std::size_t k = 0; k < mag.size(); ++k) mag[k] = std::abs(buf[k]);
    return mag;
}

}  // namespace mimicry
