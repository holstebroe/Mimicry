#pragma once
// Deterministic random numbers: splitmix64 seeding xoshiro256**. Same seed, same sequence on every
// platform, which the tests and `mimicry replay` rely on (docs/spec/01_ARCHITECTURE.md).

#include <cmath>
#include <cstdint>

namespace mimicry {

class Rng {
public:
    explicit Rng(std::uint64_t seed = 1) noexcept {
        for (auto& word : s_) {
            seed += 0x9E3779B97F4A7C15ull;
            std::uint64_t z = seed;
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
            word = z ^ (z >> 31);
        }
    }

    [[nodiscard]] std::uint64_t next_u64() noexcept {
        const std::uint64_t result = rotl(s_[1] * 5, 7) * 9;
        const std::uint64_t t = s_[1] << 17;
        s_[2] ^= s_[0];
        s_[3] ^= s_[1];
        s_[1] ^= s_[2];
        s_[0] ^= s_[3];
        s_[2] ^= t;
        s_[3] = rotl(s_[3], 45);
        return result;
    }

    // Uniform in [0, 1).
    [[nodiscard]] double uniform() noexcept { return static_cast<double>(next_u64() >> 11) * 0x1.0p-53; }
    [[nodiscard]] double uniform(double lo, double hi) noexcept { return lo + (hi - lo) * uniform(); }

    // Integer in [0, n); n > 0.
    [[nodiscard]] std::uint64_t below(std::uint64_t n) noexcept { return next_u64() % n; }

    // Standard normal (Box-Muller, the second value is cached).
    [[nodiscard]] double normal() noexcept {
        if (have_spare_) {
            have_spare_ = false;
            return spare_;
        }
        double u = uniform();
        while (u <= 0.0) u = uniform();
        const double r = std::sqrt(-2.0 * std::log(u));
        const double a = 6.283185307179586 * uniform();
        spare_ = r * std::sin(a);
        have_spare_ = true;
        return r * std::cos(a);
    }

private:
    static constexpr std::uint64_t rotl(std::uint64_t x, int k) noexcept { return (x << k) | (x >> (64 - k)); }

    std::uint64_t s_[4]{};
    double spare_ = 0;
    bool have_spare_ = false;
};

}  // namespace mimicry
