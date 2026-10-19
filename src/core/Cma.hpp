#pragma once
// CMA-ES and sep-CMA-ES (docs/spec/04_OPTIMIZER.md section 5) [S: Acidus `CMAES` class, ported], with a
// small cyclic Jacobi eigensolver instead of a linear-algebra library. Minimises; fitness values are only
// ranked, so invalid candidates can be given any value that sorts them behind the valid ones (spec 05
// section 3).

#include <cstdint>
#include <functional>
#include <vector>

#include "core/Rng.hpp"

namespace mimicry {

// Eigen-decomposition of a symmetric n x n matrix (row-major): `a = V diag(values) V^T`, eigenvectors in
// the columns of `vectors`. Cyclic Jacobi rotations; fine for the n <= 64 of full CMA-ES.
void jacobi_eigen(const std::vector<double>& a, int n, std::vector<double>& values, std::vector<double>& vectors);

struct CmaOptions {
    double sigma0 = 0.3;
    int lambda = 0;           // 0 = 4 + floor(3 ln n)
    bool separable = false;   // sep-CMA-ES: diagonal covariance (spec 04 section 5, n > 64)
    std::uint64_t seed = 1;
};

class Cma {
public:
    Cma(std::vector<double> mean, const CmaOptions& options);

    [[nodiscard]] int dimension() const noexcept { return n_; }
    [[nodiscard]] int lambda() const noexcept { return lambda_; }
    [[nodiscard]] int generation() const noexcept { return generation_; }
    [[nodiscard]] double sigma() const noexcept { return sigma_; }
    [[nodiscard]] const std::vector<double>& mean() const noexcept { return mean_; }
    [[nodiscard]] bool separable() const noexcept { return separable_; }
    // Largest over smallest eigenvalue of C.
    [[nodiscard]] double condition() const;

    // Samples `count` candidates (default: lambda).
    [[nodiscard]] std::vector<std::vector<double>> ask(int count = 0);
    // Updates the distribution from evaluated candidates (any number >= 2; lower fitness is better). The
    // candidates need not come from ask(): pre-screened or repaired points are fine.
    void tell(const std::vector<std::vector<double>>& xs, const std::vector<double>& fitness);

    // Spec 05 section 3: shrink and recentre when a generation is mostly invalid.
    void scale_sigma(double factor) noexcept { sigma_ *= factor; }
    void set_mean(std::vector<double> mean) { mean_ = std::move(mean); }

private:
    void update_eigen();

    int n_;
    int lambda_, mu_;
    bool separable_;
    std::vector<double> weights_;
    double mueff_, cc_, cs_, c1_, cmu_, damps_, chin_;
    std::vector<double> mean_, pc_, ps_;
    std::vector<double> c_;      // n x n (full) or n (diagonal)
    std::vector<double> b_, d_;  // eigenvectors (columns), sqrt of eigenvalues
    double sigma_;
    int generation_ = 0;
    int eigen_generation_ = 0;
    Rng rng_;
};

// Restart policy and stopping rules for `cma_minimize` (spec 04 section 5).
struct CmaRunOptions {
    CmaOptions cma;
    long max_evals = 10000;
    double target = -1e300;          // stop when the best fitness is at or below this
    double sigma_min = 1e-3;         // restart below this sigma [S]
    double max_condition = 1e12;     // restart above this condition number [S]
    int max_lambda = 64;             // population doubles per restart up to this cap [S]
    double restart_sigma = 0.5;      // restart sigma = sigma0 * this [S]
    int max_restarts = 9;
};

struct CmaResult {
    std::vector<double> x;
    double f = 0;
    long evals = 0;
    int restarts = 0;
};

// Minimises f with restarts from the best point (population doubled, sigma halved).
[[nodiscard]] CmaResult cma_minimize(const std::function<double(const std::vector<double>&)>& f,
                                     std::vector<double> x0, const CmaRunOptions& options);

}  // namespace mimicry
