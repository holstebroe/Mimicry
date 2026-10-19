#pragma once
// Gaussian-process surrogate for slow plugins (docs/spec/04_OPTIMIZER.md section 7) [A]. Used for
// pre-screening: CMA-ES samples several times its population, the GP ranks the samples by a lower
// confidence bound, and only the most promising lambda are rendered. Isotropic RBF kernel on the active
// subset, length scale from the median pairwise distance, targets standardised [D].

#include <cstddef>
#include <vector>

namespace mimicry::opt {

class GaussianProcess {
public:
    // Fits on points `x` (all the same dimension) with values `y`. False when there are too few points or
    // the kernel matrix is not positive definite even with jitter.
    bool fit(const std::vector<std::vector<double>>& x, const std::vector<double>& y);
    [[nodiscard]] bool ready() const noexcept { return !x_.empty(); }
    // Posterior mean and standard deviation, in the units of y.
    void predict(const std::vector<double>& x, double& mean, double& sd) const;
    [[nodiscard]] double length_scale() const noexcept { return ell_; }

private:
    [[nodiscard]] double kernel(const std::vector<double>& a, const std::vector<double>& b) const;

    std::vector<std::vector<double>> x_;
    std::vector<double> alpha_;  // K^-1 (y - mean) / scale
    std::vector<double> l_;      // Cholesky factor of K, row-major lower
    double ell_ = 1, noise_ = 1e-4, y_mean_ = 0, y_scale_ = 1;
};

// Keeps the `keep` candidates with the lowest `mean - kappa * sd` (indices into `candidates`).
[[nodiscard]] std::vector<std::size_t> prescreen(const GaussianProcess& gp, const std::vector<std::vector<double>>& candidates,
                                                 std::size_t keep, double kappa = 1.0);

}  // namespace mimicry::opt
