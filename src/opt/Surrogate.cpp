#include "opt/Surrogate.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace mimicry::opt {

double GaussianProcess::kernel(const std::vector<double>& a, const std::vector<double>& b) const {
    double d2 = 0;
    for (std::size_t i = 0; i < a.size(); ++i) d2 += (a[i] - b[i]) * (a[i] - b[i]);
    return std::exp(-0.5 * d2 / (ell_ * ell_));
}

bool GaussianProcess::fit(const std::vector<std::vector<double>>& x, const std::vector<double>& y) {
    x_.clear();
    const std::size_t m = x.size();
    if (m < 3 || y.size() != m) return false;
    // Length scale: median pairwise distance.
    std::vector<double> dist;
    for (std::size_t i = 0; i < m; ++i)
        for (std::size_t j = i + 1; j < m; ++j) {
            double d2 = 0;
            for (std::size_t k = 0; k < x[i].size(); ++k) d2 += (x[i][k] - x[j][k]) * (x[i][k] - x[j][k]);
            dist.push_back(std::sqrt(d2));
        }
    std::nth_element(dist.begin(), dist.begin() + long(dist.size() / 2), dist.end());
    ell_ = std::max(dist[dist.size() / 2], 1e-6);
    y_mean_ = std::accumulate(y.begin(), y.end(), 0.0) / double(m);
    double var = 0;
    for (double v : y) var += (v - y_mean_) * (v - y_mean_);
    y_scale_ = std::sqrt(var / double(m));
    if (!(y_scale_ > 1e-12)) y_scale_ = 1;

    for (double jitter = noise_; jitter < 1.0; jitter *= 10) {
        x_ = x;
        l_.assign(m * m, 0.0);
        bool ok = true;
        for (std::size_t i = 0; i < m && ok; ++i)
            for (std::size_t j = 0; j <= i; ++j) {
                double s = kernel(x[i], x[j]) + (i == j ? jitter : 0.0);
                for (std::size_t k = 0; k < j; ++k) s -= l_[i * m + k] * l_[j * m + k];
                if (i == j) {
                    if (!(s > 0)) {
                        ok = false;
                        break;
                    }
                    l_[i * m + i] = std::sqrt(s);
                } else {
                    l_[i * m + j] = s / l_[j * m + j];
                }
            }
        if (!ok) continue;
        // alpha = L^-T L^-1 y
        std::vector<double> z(m);
        for (std::size_t i = 0; i < m; ++i) {
            double s = (y[i] - y_mean_) / y_scale_;
            for (std::size_t k = 0; k < i; ++k) s -= l_[i * m + k] * z[k];
            z[i] = s / l_[i * m + i];
        }
        alpha_.assign(m, 0.0);
        for (std::size_t i = m; i-- > 0;) {
            double s = z[i];
            for (std::size_t k = i + 1; k < m; ++k) s -= l_[k * m + i] * alpha_[k];
            alpha_[i] = s / l_[i * m + i];
        }
        return true;
    }
    x_.clear();
    return false;
}

void GaussianProcess::predict(const std::vector<double>& x, double& mean, double& sd) const {
    const std::size_t m = x_.size();
    if (m == 0) {
        mean = 0;
        sd = 1e9;
        return;
    }
    std::vector<double> k(m);
    double mu = 0;
    for (std::size_t i = 0; i < m; ++i) {
        k[i] = kernel(x, x_[i]);
        mu += k[i] * alpha_[i];
    }
    // v = L^-1 k; var = 1 - v.v
    double vv = 0;
    std::vector<double> v(m);
    for (std::size_t i = 0; i < m; ++i) {
        double s = k[i];
        for (std::size_t j = 0; j < i; ++j) s -= l_[i * m + j] * v[j];
        v[i] = s / l_[i * m + i];
        vv += v[i] * v[i];
    }
    mean = y_mean_ + y_scale_ * mu;
    sd = y_scale_ * std::sqrt(std::max(1.0 - vv, 0.0));
}

std::vector<std::size_t> prescreen(const GaussianProcess& gp, const std::vector<std::vector<double>>& candidates,
                                   std::size_t keep, double kappa) {
    std::vector<double> score(candidates.size());
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        double m = 0, s = 0;
        gp.predict(candidates[i], m, s);
        score[i] = m - kappa * s;
    }
    std::vector<std::size_t> idx(candidates.size());
    std::iota(idx.begin(), idx.end(), 0);
    std::stable_sort(idx.begin(), idx.end(), [&](std::size_t a, std::size_t b) { return score[a] < score[b]; });
    idx.resize(std::min(keep, idx.size()));
    return idx;
}

}  // namespace mimicry::opt
