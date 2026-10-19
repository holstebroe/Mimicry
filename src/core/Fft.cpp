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
    // Per-stage twiddles, contiguous: the stage with half-length h reads stage_tw_[h - 1 .. 2h - 2].
    stage_tw_.resize(n_ - 1);
    for (std::size_t h = 1; h < n_; h <<= 1)
        for (std::size_t j = 0; j < h; ++j) stage_tw_[h - 1 + j] = twiddle_[j * (n_ / (2 * h))];
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
    // Plain real arithmetic: std::complex multiplication goes through a NaN-checking libcall without
    // -fcx-limited-range, which made this loop several times slower.
    const double sign = inverse ? -1.0 : 1.0;
    for (std::size_t len = 2; len <= n_; len <<= 1) {
        const std::size_t half = len / 2;
        const std::complex<double>* tw = &stage_tw_[half - 1];
        for (std::size_t i = 0; i < n_; i += len) {
            for (std::size_t j = 0; j < half; ++j) {
                const double wr = tw[j].real(), wi = sign * tw[j].imag();
                const double xr = d[i + j + half].real(), xi = d[i + j + half].imag();
                const double vr = xr * wr - xi * wi, vi = xr * wi + xi * wr;
                const double ur = d[i + j].real(), ui = d[i + j].imag();
                d[i + j] = {ur + vr, ui + vi};
                d[i + j + half] = {ur - vr, ui - vi};
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
