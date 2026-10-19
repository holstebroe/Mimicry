#include "core/Cma.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace mimicry {

void jacobi_eigen(const std::vector<double>& a_in, int n, std::vector<double>& values, std::vector<double>& vectors) {
    const std::size_t N = static_cast<std::size_t>(n);
    std::vector<double> a = a_in;
    vectors.assign(N * N, 0.0);
    for (std::size_t i = 0; i < N; ++i) vectors[i * N + i] = 1.0;
    for (int sweep = 0; sweep < 100; ++sweep) {
        double off = 0, diag = 0;
        for (std::size_t i = 0; i < N; ++i) {
            diag += a[i * N + i] * a[i * N + i];
            for (std::size_t j = i + 1; j < N; ++j) off += a[i * N + j] * a[i * N + j];
        }
        if (off <= 1e-30 * (diag + 1e-300)) break;
        for (std::size_t p = 0; p < N; ++p) {
            for (std::size_t q = p + 1; q < N; ++q) {
                const double apq = a[p * N + q];
                if (std::abs(apq) < 1e-300) continue;
                const double theta = (a[q * N + q] - a[p * N + p]) / (2.0 * apq);
                const double t = (theta >= 0 ? 1.0 : -1.0) / (std::abs(theta) + std::sqrt(theta * theta + 1.0));
                const double c = 1.0 / std::sqrt(t * t + 1.0), s = t * c;
                for (std::size_t k = 0; k < N; ++k) {  // A <- A J (columns p, q)
                    const double akp = a[k * N + p], akq = a[k * N + q];
                    a[k * N + p] = c * akp - s * akq;
                    a[k * N + q] = s * akp + c * akq;
                }
                for (std::size_t k = 0; k < N; ++k) {  // A <- J^T A (rows p, q)
                    const double apk = a[p * N + k], aqk = a[q * N + k];
                    a[p * N + k] = c * apk - s * aqk;
                    a[q * N + k] = s * apk + c * aqk;
                }
                for (std::size_t k = 0; k < N; ++k) {
                    const double vkp = vectors[k * N + p], vkq = vectors[k * N + q];
                    vectors[k * N + p] = c * vkp - s * vkq;
                    vectors[k * N + q] = s * vkp + c * vkq;
                }
            }
        }
    }
    values.resize(N);
    for (std::size_t i = 0; i < N; ++i) values[i] = a[i * N + i];
}

Cma::Cma(std::vector<double> mean, const CmaOptions& options)
    : n_(static_cast<int>(mean.size())), separable_(options.separable), mean_(std::move(mean)),
      sigma_(options.sigma0), rng_(options.seed) {
    const double n = std::max(1, n_);
    lambda_ = options.lambda > 0 ? options.lambda : 4 + static_cast<int>(std::floor(3.0 * std::log(n)));
    lambda_ = std::max(lambda_, 4);
    mu_ = lambda_ / 2;
    weights_.resize(static_cast<std::size_t>(mu_));
    for (int i = 0; i < mu_; ++i) weights_[std::size_t(i)] = std::log(mu_ + 0.5) - std::log(i + 1.0);
    const double wsum = std::accumulate(weights_.begin(), weights_.end(), 0.0);
    double w2 = 0;
    for (auto& w : weights_) {
        w /= wsum;
        w2 += w * w;
    }
    mueff_ = 1.0 / w2;
    cc_ = (4 + mueff_ / n) / (n + 4 + 2 * mueff_ / n);
    cs_ = (mueff_ + 2) / (n + mueff_ + 5);
    c1_ = 2 / ((n + 1.3) * (n + 1.3) + mueff_);
    cmu_ = std::min(1 - c1_, 2 * (mueff_ - 2 + 1 / mueff_) / ((n + 2) * (n + 2) + mueff_));
    if (separable_) {
        // Ros and Hansen 2008: the diagonal model learns (n + 2) / 3 times faster.
        c1_ = std::min(1.0, c1_ * (n + 2) / 3);
        cmu_ = std::min(1 - c1_, cmu_ * (n + 2) / 3);
    }
    damps_ = 1 + 2 * std::max(0.0, std::sqrt((mueff_ - 1) / (n + 1)) - 1) + cs_;
    chin_ = std::sqrt(n) * (1 - 1 / (4 * n) + 1 / (21 * n * n));
    const std::size_t N = static_cast<std::size_t>(n_);
    pc_.assign(N, 0.0);
    ps_.assign(N, 0.0);
    d_.assign(N, 1.0);
    if (separable_) {
        c_.assign(N, 1.0);
    } else {
        c_.assign(N * N, 0.0);
        b_.assign(N * N, 0.0);
        for (std::size_t i = 0; i < N; ++i) c_[i * N + i] = b_[i * N + i] = 1.0;
    }
}

double Cma::condition() const {
    if (d_.empty()) return 1.0;
    const auto [lo, hi] = std::minmax_element(d_.begin(), d_.end());
    return *lo > 0 ? (*hi * *hi) / (*lo * *lo) : 1e300;
}

std::vector<std::vector<double>> Cma::ask(int count) {
    if (count <= 0) count = lambda_;
    const std::size_t N = static_cast<std::size_t>(n_);
    std::vector<std::vector<double>> out(static_cast<std::size_t>(count), std::vector<double>(N));
    std::vector<double> z(N);
    for (auto& x : out) {
        for (auto& v : z) v = rng_.normal();
        for (std::size_t i = 0; i < N; ++i) {
            double y = 0;
            if (separable_) {
                y = d_[i] * z[i];
            } else {
                for (std::size_t j = 0; j < N; ++j) y += b_[i * N + j] * d_[j] * z[j];
            }
            x[i] = mean_[i] + sigma_ * y;
        }
    }
    return out;
}

void Cma::tell(const std::vector<std::vector<double>>& xs, const std::vector<double>& fitness) {
    const std::size_t N = static_cast<std::size_t>(n_);
    if (xs.size() < 2 || xs.size() != fitness.size() || N == 0) return;
    std::vector<std::size_t> order(xs.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return fitness[a] < fitness[b]; });
    // Recombination over the best mu (fewer if fewer candidates were told).
    const int mu = std::min<int>(mu_, static_cast<int>(xs.size()));
    std::vector<double> w(weights_.begin(), weights_.begin() + mu);
    const double ws = std::accumulate(w.begin(), w.end(), 0.0);
    for (auto& v : w) v /= ws;

    std::vector<std::vector<double>> y(static_cast<std::size_t>(mu), std::vector<double>(N));
    std::vector<double> yw(N, 0.0);
    for (int k = 0; k < mu; ++k) {
        const auto& x = xs[order[std::size_t(k)]];
        for (std::size_t i = 0; i < N; ++i) {
            y[std::size_t(k)][i] = (x[i] - mean_[i]) / sigma_;
            yw[i] += w[std::size_t(k)] * y[std::size_t(k)][i];
        }
    }
    for (std::size_t i = 0; i < N; ++i) mean_[i] += sigma_ * yw[i];

    // C^{-1/2} yw
    std::vector<double> cinv(N, 0.0);
    if (separable_) {
        for (std::size_t i = 0; i < N; ++i) cinv[i] = yw[i] / d_[i];
    } else {
        std::vector<double> t(N, 0.0);
        for (std::size_t j = 0; j < N; ++j) {
            double s = 0;
            for (std::size_t i = 0; i < N; ++i) s += b_[i * N + j] * yw[i];
            t[j] = s / d_[j];
        }
        for (std::size_t i = 0; i < N; ++i) {
            double s = 0;
            for (std::size_t j = 0; j < N; ++j) s += b_[i * N + j] * t[j];
            cinv[i] = s;
        }
    }
    const double csn = std::sqrt(cs_ * (2 - cs_) * mueff_);
    double psn = 0;
    for (std::size_t i = 0; i < N; ++i) {
        ps_[i] = (1 - cs_) * ps_[i] + csn * cinv[i];
        psn += ps_[i] * ps_[i];
    }
    psn = std::sqrt(psn);
    ++generation_;
    const double n = static_cast<double>(N);
    const bool hsig = psn / std::sqrt(1 - std::pow(1 - cs_, 2.0 * generation_)) / chin_ < 1.4 + 2 / (n + 1);
    const double ccn = std::sqrt(cc_ * (2 - cc_) * mueff_);
    for (std::size_t i = 0; i < N; ++i) pc_[i] = (1 - cc_) * pc_[i] + (hsig ? ccn * yw[i] : 0.0);
    const double dh = hsig ? 0.0 : cc_ * (2 - cc_);

    if (separable_) {
        for (std::size_t i = 0; i < N; ++i) {
            double rank_mu = 0;
            for (int k = 0; k < mu; ++k) rank_mu += w[std::size_t(k)] * y[std::size_t(k)][i] * y[std::size_t(k)][i];
            c_[i] = (1 - c1_ - cmu_) * c_[i] + c1_ * (pc_[i] * pc_[i] + dh * c_[i]) + cmu_ * rank_mu;
            c_[i] = std::max(c_[i], 1e-300);
            d_[i] = std::sqrt(c_[i]);
        }
    } else {
        for (std::size_t i = 0; i < N; ++i) {
            for (std::size_t j = 0; j <= i; ++j) {
                double rank_mu = 0;
                for (int k = 0; k < mu; ++k) rank_mu += w[std::size_t(k)] * y[std::size_t(k)][i] * y[std::size_t(k)][j];
                const double v = (1 - c1_ - cmu_) * c_[i * N + j] + c1_ * (pc_[i] * pc_[j] + dh * c_[i * N + j]) + cmu_ * rank_mu;
                c_[i * N + j] = c_[j * N + i] = v;
            }
        }
    }
    sigma_ *= std::exp(std::min(1.0, (cs_ / damps_) * (psn / chin_ - 1)));
    if (!separable_ && generation_ - eigen_generation_ > lambda_ / (c1_ + cmu_) / n / 10) update_eigen();
}

void Cma::update_eigen() {
    eigen_generation_ = generation_;
    std::vector<double> values;
    jacobi_eigen(c_, n_, values, b_);
    for (std::size_t i = 0; i < values.size(); ++i) d_[i] = std::sqrt(std::max(values[i], 1e-300));
}

bool cma_should_restart(const Cma& cma, const CmaRunOptions& options) {
    return cma.sigma() < options.sigma_min || cma.condition() > options.max_condition;
}

CmaOptions cma_restart_options(const CmaRunOptions& options, int restart, int previous_lambda) {
    CmaOptions co = options.cma;
    co.seed = options.cma.seed + static_cast<std::uint64_t>(restart) * 7919u;
    if (restart > 0) {
        co.sigma0 = options.cma.sigma0 * options.restart_sigma;
        co.lambda = std::min(options.max_lambda, previous_lambda * 2);
    }
    return co;
}

CmaResult cma_minimize(const std::function<double(const std::vector<double>&)>& f, std::vector<double> x0,
                       const CmaRunOptions& options) {
    CmaResult best;
    best.x = x0;
    best.f = f(x0);
    best.evals = 1;
    int lambda = 0;
    for (int restart = 0; restart <= options.max_restarts && best.evals < options.max_evals; ++restart) {
        Cma cma(best.x, cma_restart_options(options, restart, lambda));
        while (best.evals < options.max_evals && best.f > options.target) {
            auto xs = cma.ask();
            std::vector<double> fs(xs.size());
            for (std::size_t i = 0; i < xs.size(); ++i) {
                fs[i] = f(xs[i]);
                if (fs[i] < best.f) {
                    best.f = fs[i];
                    best.x = xs[i];
                }
            }
            best.evals += static_cast<long>(xs.size());
            cma.tell(xs, fs);
            if (cma_should_restart(cma, options)) break;
        }
        if (best.f <= options.target) break;
        best.restarts = restart + 1;
        lambda = cma.lambda();
    }
    return best;
}

}  // namespace mimicry
