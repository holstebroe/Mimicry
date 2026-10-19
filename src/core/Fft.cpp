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

void Fft::transform(std::vector<std::complex<double>>& data, bool inverse) const {
    if (data.size() != n_) throw std::invalid_argument("Fft: wrong buffer size");
    for (std::size_t i = 0; i < n_; ++i)
        if (i < rev_[i]) std::swap(data[i], data[rev_[i]]);
    // Plain real arithmetic on the interleaved (re, im) array: std::complex multiplication goes through a
    // NaN-checking libcall without -fcx-limited-range, which made this loop several times slower.
    double* d = reinterpret_cast<double*>(data.data());
    const double sign = inverse ? -1.0 : 1.0;
    // Stages 1 and 2 have trivial twiddles (1, and -i / +i): no multiplications.
    for (std::size_t i = 0; i < 2 * n_; i += 4) {
        const double ar = d[i], ai = d[i + 1], br = d[i + 2], bi = d[i + 3];
        d[i] = ar + br, d[i + 1] = ai + bi, d[i + 2] = ar - br, d[i + 3] = ai - bi;
    }
    if (n_ >= 4) {
        for (std::size_t i = 0; i < 2 * n_; i += 8) {
            // j = 0: w = 1
            double ur = d[i], ui = d[i + 1], vr = d[i + 4], vi = d[i + 5];
            d[i] = ur + vr, d[i + 1] = ui + vi, d[i + 4] = ur - vr, d[i + 5] = ui - vi;
            // j = 1: w = -i (forward) or +i (inverse): v = x * w
            const double xr = d[i + 6], xi = d[i + 7];
            vr = sign * xi, vi = -sign * xr;
            ur = d[i + 2], ui = d[i + 3];
            d[i + 2] = ur + vr, d[i + 3] = ui + vi, d[i + 6] = ur - vr, d[i + 7] = ui - vi;
        }
    }
    for (std::size_t len = 8; len <= n_; len <<= 1) {
        const std::size_t half = len / 2;
        const double* tw = reinterpret_cast<const double*>(&stage_tw_[half - 1]);
        for (std::size_t i = 0; i < n_; i += len) {
            double* a = d + 2 * i;
            double* b = d + 2 * (i + half);
            for (std::size_t j = 0; j < half; ++j) {
                const double wr = tw[2 * j], wi = sign * tw[2 * j + 1];
                const double xr = b[2 * j], xi = b[2 * j + 1];
                const double vr = xr * wr - xi * wi, vi = xr * wi + xi * wr;
                const double ur = a[2 * j], ui = a[2 * j + 1];
                a[2 * j] = ur + vr, a[2 * j + 1] = ui + vi;
                b[2 * j] = ur - vr, b[2 * j + 1] = ui - vi;
            }
        }
    }
    if (inverse) {
        const double s = 1.0 / static_cast<double>(n_);
        for (std::size_t i = 0; i < 2 * n_; ++i) d[i] *= s;
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
