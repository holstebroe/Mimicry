#pragma once
// Shared spectral helpers for the feature extractors (docs/spec/03_FEATURES_LOSS.md). Everything works on
// mono float signals and power spectra; dB values are 10*log10(power) with a hard floor.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "core/Fft.hpp"

namespace mimicry::features {

inline constexpr float kFloorDb = -120.0f;

[[nodiscard]] float power_to_db(double power) noexcept;

// 10*log10(p) from the float's exponent and an atanh series; within 1e-4 dB and several times faster than
// std::log10 on the large spectrogram grids. Floors at kFloorDb like power_to_db.
[[nodiscard]] inline float fast_power_db(float p) noexcept {
    if (!(p > 1e-12f)) return kFloorDb;
    std::uint32_t bits;
    std::memcpy(&bits, &p, sizeof bits);
    int e = static_cast<int>((bits >> 23) & 0xFF) - 127;
    bits = (bits & 0x7FFFFFu) | 0x3F800000u;
    float m;
    std::memcpy(&m, &bits, sizeof m);
    if (m > 1.41421356f) {
        m *= 0.5f;
        ++e;
    }
    const float z = (m - 1.0f) / (m + 1.0f), z2 = z * z;
    const float l = z * (1.0f + z2 * (1.0f / 3.0f + z2 * (1.0f / 5.0f + z2 * (1.0f / 7.0f))));
    return 3.01029996f * (static_cast<float>(e) + 2.88539008f * l);
}

// Hann-windowed power spectrogram with a fixed window and FFT size. Two real frames share one complex
// FFT, so a frame costs half a transform. Power is normalised by the window sum, so a full-scale sine of
// amplitude A has a peak of about A^2/4. Not thread-safe to share (create one per thread).
class PowerStft {
public:
    PowerStft(std::size_t window, std::size_t fft_size);

    [[nodiscard]] std::size_t window() const noexcept { return window_.size(); }
    [[nodiscard]] std::size_t bins() const noexcept { return fft_.size() / 2 + 1; }
    [[nodiscard]] std::size_t fft_size() const noexcept { return fft_.size(); }

    // Number of full frames of `window` samples at `hop`; at least 1 (a short signal is zero padded).
    [[nodiscard]] std::size_t frames(std::size_t length, std::size_t hop) const noexcept;

    // frames x bins, row-major, linear power.
    [[nodiscard]] std::vector<float> power(const float* x, std::size_t length, std::size_t hop);

    // Power spectrum of a single frame starting at `start` (zero padded past the end).
    void frame_power(const float* x, std::size_t length, std::size_t start, float* out);

private:
    void load(const float* x, std::size_t length, std::size_t start, std::vector<std::complex<double>>& buf,
              bool imaginary) const;

    Fft fft_;
    std::vector<double> window_;
    double norm_ = 1;
    std::vector<std::complex<double>> buf_;
};

[[nodiscard]] std::size_t next_pow2(std::size_t n) noexcept;

}  // namespace mimicry::features
