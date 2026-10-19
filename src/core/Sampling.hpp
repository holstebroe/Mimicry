#pragma once
// Space-filling samples in the unit cube for the S1 seed stage (docs/spec/04_OPTIMIZER.md section 4).
// Latin hypercube: every dimension is cut into `count` strata and each stratum holds exactly one point,
// so even a few hundred samples cover every parameter's whole range [D: LHS rather than Sobol, which
// would need direction-number tables for hundreds of dimensions].

#include <cstddef>
#include <vector>

#include "core/Rng.hpp"

namespace mimicry {

// `count` points of dimension `dims`, each coordinate in [0, 1).
[[nodiscard]] std::vector<std::vector<double>> latin_hypercube(std::size_t count, std::size_t dims, Rng& rng);

}  // namespace mimicry
