#pragma once
// Radix-2 complex FFT and a real-input helper. Sizes are powers of two. Plans are reusable and not
// thread-safe to share while running (create one per thread).

#include <complex>
#include <vector>

namespace mimicry {

class Fft {
public:
    explicit Fft(std::size_t size);  // size must be a power of two >= 2

    [[nodiscard]] std::size_t size() const noexcept { return n_; }

    // In-place forward transform (e^{-i...}); unnormalised.
    void forward(std::vector<std::complex<double>>& data) const;
    // In-place inverse transform, scaled by 1/N.
    void inverse(std::vector<std::complex<double>>& data) const;

    // Magnitude spectrum of a real signal of `size()` samples: size()/2 + 1 bins.
    [[nodiscard]] std::vector<double> magnitude(const float* signal) const;

    [[nodiscard]] static bool is_power_of_two(std::size_t n) noexcept { return n >= 2 && (n & (n - 1)) == 0; }

private:
    void transform(std::vector<std::complex<double>>& data, bool inverse) const;

    std::size_t n_;
    std::vector<std::complex<double>> twiddle_;
    std::vector<std::complex<double>> stage_tw_;
    std::vector<std::size_t> rev_;
};

}  // namespace mimicry
