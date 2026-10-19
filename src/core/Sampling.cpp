#include "core/Sampling.hpp"

#include <numeric>
#include <utility>

namespace mimicry {

std::vector<std::vector<double>> latin_hypercube(std::size_t count, std::size_t dims, Rng& rng) {
    std::vector<std::vector<double>> pts(count, std::vector<double>(dims));
    std::vector<std::size_t> perm(count);
    for (std::size_t d = 0; d < dims; ++d) {
        std::iota(perm.begin(), perm.end(), 0);
        for (std::size_t i = count; i > 1; --i) std::swap(perm[i - 1], perm[rng.below(i)]);  // Fisher-Yates
        for (std::size_t i = 0; i < count; ++i)
            pts[i][d] = (static_cast<double>(perm[i]) + rng.uniform()) / static_cast<double>(count);
    }
    return pts;
}

}  // namespace mimicry
