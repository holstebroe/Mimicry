#include "opt/Pipeline.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <numeric>

#include "core/Cma.hpp"
#include "core/Sampling.hpp"
#include "opt/Surrogate.hpp"

namespace mimicry::opt {

namespace {

using Clock = std::chrono::steady_clock;

constexpr double kInvalidBase = 1e6;  // invalid candidates rank after every valid one (spec 05 section 3)

struct Budget {
    double deadline_s = 0;  // since start
    long eval_limit = 0;    // absolute evaluation count; 0 = none
};

class Runner {
public:
    Runner(Objective& objective, const Space& space, const PipelineOptions& options)
        : obj_(objective), space_(space), opt_(options), start_(Clock::now()), rng_(options.seed) {}

    PipelineResult run(const std::vector<std::vector<double>>& seeds);

private:
    [[nodiscard]] double now() const { return std::chrono::duration<double>(Clock::now() - start_).count(); }
    [[nodiscard]] bool enabled(int stage) const {
        return opt_.stages.empty() || std::find(opt_.stages.begin(), opt_.stages.end(), stage) != opt_.stages.end();
    }
    [[nodiscard]] bool global_stop() const {
        return now() >= opt_.time_cap_s || (opt_.max_evals > 0 && res_.evals >= opt_.max_evals);
    }
    [[nodiscard]] bool stop(const Budget& b) const {
        return global_stop() || now() >= b.deadline_s || (b.eval_limit > 0 && res_.evals >= b.eval_limit);
    }
    // A share of what is left of the time cap and the evaluation budget.
    [[nodiscard]] Budget share(double fraction) const {
        Budget b;
        const double t = now();
        b.deadline_s = t + fraction * std::max(0.0, opt_.time_cap_s - t);
        if (opt_.max_evals > 0)
            b.eval_limit = res_.evals + std::max(1L, long(fraction * double(std::max(0L, opt_.max_evals - res_.evals))));
        return b;
    }

    [[nodiscard]] double prior_term(const std::vector<double>& coords) const {
        if (!opt_.prior) return 0;
        const double scale = std::isfinite(best_.eval.loss) ? std::max(best_.eval.loss, 0.1) : 1.0;
        return opt_.prior_fraction * scale * space_.prior(coords);
    }

    // Evaluates full vectors; `penalty` is the boundary clip distance of each (may be empty). Returns
    // candidates with fitness = loss + boundary + prior for valid ones and +inf for invalid ones.
    std::vector<Candidate> evaluate(const std::vector<std::vector<double>>& coords, const std::vector<double>& penalty = {});
    void note_progress(const std::string& stage);

    // One CMA-ES run over `free` from `start` with restarts (spec 04 section 5). Returns its best.
    Candidate run_cma(const std::string& name, const std::vector<std::size_t>& free, const Candidate& start,
                      double sigma, const Budget& budget, std::uint64_t seed);

    void stage_seed();
    // One categorical at a time on the best point, keeping a change that improves it (up to two passes).
    void sweep_categorical();
    void stage_groups();
    void stage_polish();
    void stage_categorical();
    void stage_refine();
    void select_top_k();

    void begin_stage(const std::string& name, int free) {
        StageReport r;
        r.name = name;
        r.free = free;
        r.before = best_.eval.loss;
        r.evals = res_.evals;
        r.seconds = now();
        res_.stages.push_back(r);
        stage_ = name;
        if (opt_.log) opt_.log(std::format("{}: {} free, best {:.3f} dB", name, free, best_.eval.loss));
    }
    void end_stage() {
        auto& r = res_.stages.back();
        r.after = best_.eval.loss;
        r.evals = res_.evals - r.evals;
        r.seconds = now() - r.seconds;
    }
    void remember(const Candidate& c);

    Objective& obj_;
    const Space& space_;
    const PipelineOptions& opt_;
    Clock::time_point start_;
    Rng rng_;
    PipelineResult res_;
    Candidate best_;
    std::string stage_;
    double last_progress_ = 0;
    std::vector<Candidate> kept_;       // S1 best K, starting points of the S5 restarts
    std::vector<Candidate> finalists_;  // candidates for the top-K set
    std::vector<std::pair<std::vector<double>, double>> archive_;  // valid (coords, fitness): surrogate data
};

std::vector<Candidate> Runner::evaluate(const std::vector<std::vector<double>>& coords, const std::vector<double>& penalty) {
    auto evals = obj_.evaluate(coords, false);
    evals.resize(coords.size());
    std::vector<Candidate> out(coords.size());
    for (std::size_t i = 0; i < coords.size(); ++i) {
        Candidate& c = out[i];
        c.coords = coords[i];
        c.eval = std::move(evals[i]);
        ++res_.outcomes[c.eval.outcome];
        if (c.eval.ok() && std::isfinite(c.eval.loss)) {
            const double base = c.eval.loss + prior_term(c.coords);
            if (!std::isfinite(best_.eval.loss) || base < best_.fitness) {
                best_ = c;
                best_.fitness = base;
            }
            c.fitness = base + (i < penalty.size() ? opt_.boundary_weight * penalty[i] : 0.0);
            archive_.emplace_back(c.coords, base);
            if (archive_.size() > 400) archive_.erase(archive_.begin(), archive_.begin() + 100);
        } else {
            c.fitness = std::numeric_limits<double>::infinity();
        }
    }
    res_.evals += long(coords.size());
    note_progress(stage_);
    return out;
}

void Runner::note_progress(const std::string& stage) {
    if (!opt_.progress || now() - last_progress_ < opt_.progress_interval_s) return;
    last_progress_ = now();
    opt_.progress({stage, res_.evals, best_.eval.loss, now()});
}

void Runner::remember(const Candidate& c) {
    if (!c.eval.ok() || !std::isfinite(c.eval.loss)) return;
    for (const auto& f : finalists_)
        if (space_.distance(f.coords, c.coords) < 1e-9) return;
    finalists_.push_back(c);
}

Candidate Runner::run_cma(const std::string& name, const std::vector<std::size_t>& free, const Candidate& start,
                          double sigma0, const Budget& budget, std::uint64_t seed) {
    Candidate run_best = start;
    if (free.empty()) return run_best;
    const std::size_t n = free.size();
    CmaRunOptions ro;  // restart policy and its [S] constants (core/Cma.hpp)
    ro.cma.sigma0 = sigma0;
    ro.cma.separable = int(n) > opt_.sep_threshold;
    ro.cma.seed = seed;
    const bool surrogate = opt_.surrogate_above_s == 0 ||
                           (opt_.surrogate_above_s > 0 && obj_.seconds_per_candidate() > opt_.surrogate_above_s);
    if (surrogate) res_.surrogate_used = true;
    // Searched parameters this run holds at run_best's values: the surrogate learns only from archive
    // points that agree on them, or points differing only there would project onto one x with different y.
    std::vector<std::size_t> held;
    for (auto list : {&space_.active(), &space_.categorical()})
        for (auto i : *list)
            if (std::find(free.begin(), free.end(), i) == free.end()) held.push_back(i);
    int lambda = 0;
    double last_improve = now();
    for (int restart = 0; restart <= ro.max_restarts && !stop(budget); ++restart) {
        Cma cma(space_.encode(free, run_best.coords), cma_restart_options(ro, restart, lambda));
        bool restart_now = false;
        while (!stop(budget) && !restart_now) {
            std::vector<std::vector<double>> xs;
            std::vector<std::vector<double>> gx;
            std::vector<double> gy;
            if (surrogate) {
                for (std::size_t i = archive_.size(); i-- > 0 && gx.size() < 150;) {
                    const auto& c = archive_[i].first;
                    bool same = true;
                    for (auto h : held) same = same && std::abs(c[h] - run_best.coords[h]) < 1e-9;
                    if (!same) continue;
                    gx.push_back(space_.encode(free, c));
                    gy.push_back(archive_[i].second);
                }
            }
            if (surrogate && gx.size() >= std::max<std::size_t>(10, 2 * n + 1)) {
                // Pre-screen 3 lambda samples by the GP's lower confidence bound (spec 04 section 7).
                GaussianProcess gp;
                auto pool = cma.ask(3 * cma.lambda());
                if (gp.fit(gx, gy)) {
                    for (auto i : prescreen(gp, pool, std::size_t(cma.lambda()))) xs.push_back(pool[i]);
                } else {
                    pool.resize(std::size_t(cma.lambda()));
                    xs = std::move(pool);
                }
            } else {
                xs = cma.ask();
            }
            std::vector<std::vector<double>> coords(xs.size(), run_best.coords);
            std::vector<double> pen(xs.size());
            for (std::size_t i = 0; i < xs.size(); ++i) pen[i] = space_.decode(free, xs[i], coords[i]);
            auto cands = evaluate(coords, pen);

            // Rank invalid candidates behind the valid ones: by severity, then by distance to the run's best
            // valid point, so the strategy still gets a direction away from the hazard (spec 05 section 3).
            const auto best_x = space_.encode(free, run_best.coords);
            std::vector<double> fit(xs.size());
            int invalid = 0;
            for (std::size_t i = 0; i < xs.size(); ++i) {
                if (std::isfinite(cands[i].fitness)) {
                    fit[i] = cands[i].fitness;
                    const double base = cands[i].eval.loss + prior_term(cands[i].coords);
                    if (base < run_best.fitness - 1e-12 || !std::isfinite(run_best.eval.loss)) {
                        run_best = cands[i];
                        run_best.fitness = base;
                        last_improve = now();
                    }
                } else {
                    ++invalid;
                    double d = 0;
                    for (std::size_t k = 0; k < n; ++k) d += (xs[i][k] - best_x[k]) * (xs[i][k] - best_x[k]);
                    fit[i] = kInvalidBase + 1e4 * severity(cands[i].eval.outcome) + 100.0 * std::sqrt(d);
                }
            }
            cma.tell(xs, fit);
            if (2 * invalid > int(xs.size()) && std::isfinite(run_best.eval.loss)) {
                cma.scale_sigma(opt_.invalid_shrink);
                cma.set_mean(space_.encode(free, run_best.coords));
            }
            if (now() - last_improve > opt_.patience_s) {
                if (opt_.log) opt_.log(std::format("{}: no improvement for {:.0f} s, stopping", name, opt_.patience_s));
                return run_best;
            }
            restart_now = cma_should_restart(cma, ro);
            lambda = cma.lambda();
        }
        // The next run restarts from the best point, sigma0 halved, population doubled (cap 64) [S].
    }
    return run_best;
}

void Runner::stage_seed() {
    const auto& active = space_.active();
    begin_stage("S1 seed", int(active.size() + space_.categorical().size()));
    long n = opt_.seed_samples;
    if (opt_.max_evals > 0) n = std::min(n, std::max(1L, opt_.max_evals / 4));
    const auto lhs = latin_hypercube(std::size_t(std::max(0L, n)), active.size(), rng_);
    std::vector<std::vector<double>> pts;
    for (const auto& u : lhs) pts.push_back(space_.sample_box(u, rng_));
    const std::size_t batch = 64;
    std::vector<Candidate> all;
    for (std::size_t i = 0; i < pts.size() && !global_stop(); i += batch) {
        std::vector<std::vector<double>> b(pts.begin() + long(i), pts.begin() + long(std::min(pts.size(), i + batch)));
        for (auto& c : evaluate(b)) all.push_back(std::move(c));
    }
    std::stable_sort(all.begin(), all.end(), [](const Candidate& a, const Candidate& b) { return a.fitness < b.fitness; });
    // The defaults and the given seeds were scored before S1; keep the best K distinct points of all.
    for (auto& c : all) {
        if (!std::isfinite(c.fitness) || int(kept_.size()) >= opt_.keep_best) break;
        bool dup = false;
        for (const auto& k : kept_) dup = dup || space_.distance(k.coords, c.coords) < 0.05;
        if (!dup) kept_.push_back(c);
    }
    std::stable_sort(kept_.begin(), kept_.end(), [](const Candidate& a, const Candidate& b) { return a.fitness < b.fitness; });
    sweep_categorical();
    end_stage();
}

void Runner::sweep_categorical() {
    for (int pass = 0; pass < 2 && !global_stop() && std::isfinite(best_.eval.loss); ++pass) {
        std::vector<std::vector<double>> xs;
        for (auto c : space_.categorical())
            for (double v : space_.values(c)) {
                if (std::abs(v - best_.coords[c]) < 1e-9) continue;
                auto x = best_.coords;
                x[c] = v;
                xs.push_back(std::move(x));
            }
        if (xs.empty()) return;
        const double before = best_.fitness;
        (void)evaluate(xs);  // evaluate() adopts an improvement as the new best
        if (!(best_.fitness < before)) return;
        if (opt_.log) opt_.log(std::format("{}: categorical sweep {:.3f} -> {:.3f} dB", stage_, before, best_.fitness));
    }
}

void Runner::stage_groups() {
    std::vector<std::size_t> prev;
    for (int s = 2; s <= 4 && !global_stop(); ++s) {
        if (!enabled(s)) continue;
        std::vector<std::size_t> free;
        for (auto i : space_.active())
            if (stage_of(space_.slot(i).group) <= s) free.push_back(i);
        if (free.empty() || free == prev) continue;
        prev = free;
        static constexpr const char* kNames[] = {"", "", "S2 osc+amp", "S3 +filter+env", "S4 +lfo+fx"};
        begin_stage(kNames[s], int(free.size()));
        const Candidate start = best_;
        const auto got = run_cma(kNames[s], free, start, opt_.sigma_stage, share(0.12), opt_.seed + std::uint64_t(s) * 1000u);
        remember(got);
        sweep_categorical();
        end_stage();
    }
}

void Runner::stage_polish() {
    const auto& active = space_.active();
    if (active.empty()) return;
    begin_stage(int(active.size()) > opt_.sep_threshold ? "S5 polish (sep-CMA-ES)" : "S5 polish (CMA-ES)", int(active.size()));
    const int R = std::max(1, opt_.restarts);
    const double total = 0.75;  // of what is left; S6 and S7 share the rest
    const Budget all = share(total);
    std::vector<std::vector<double>> starts;  // where earlier runs began: a seed starts at most one run
    for (int r = 0; r < R && !global_stop(); ++r) {
        // Run 0 polishes the best so far; the others start from the next S1 seeds no run has used.
        Candidate start = best_;
        double sigma = opt_.sigma_polish;
        if (r > 0 && std::size_t(r) < kept_.size() + 1) {
            for (const auto& k : kept_) {
                bool used = space_.distance(k.coords, best_.coords) < 0.05;
                for (const auto& f : finalists_) used = used || space_.distance(k.coords, f.coords) < 0.05;
                for (const auto& st : starts) used = used || space_.distance(k.coords, st) < 0.05;
                if (!used) {
                    start = k;
                    sigma = opt_.sigma_stage;
                    break;
                }
            }
        }
        // Run 1 polishes the best point and gets half of S5 [D]; the other runs split the rest.
        const double part = R == 1 ? 1.0 : r == 0 ? 0.5 : 1.0 / double(R - r);
        Budget b;
        b.deadline_s = now() + part * (all.deadline_s - now());
        if (all.eval_limit > 0) b.eval_limit = res_.evals + std::max(1L, long(part * double(all.eval_limit - res_.evals)));
        starts.push_back(start.coords);
        const auto got = run_cma(std::format("S5 run {}", r + 1), active, start, sigma, b, opt_.seed + 5000u + std::uint64_t(r) * 104729u);
        remember(got);
    }
    end_stage();
}

void Runner::stage_categorical() {
    std::vector<std::size_t> cats;
    for (auto c : space_.categorical()) {
        const auto v = space_.values(c);
        if (v.size() >= 2 && int(v.size()) <= opt_.max_categorical_values) cats.push_back(c);
    }
    if (cats.empty() || !std::isfinite(best_.eval.loss)) return;
    begin_stage("S6 categorical", int(cats.size()));
    // Importance: the loss spread when one categorical changes on the best point.
    std::vector<std::vector<double>> probe;
    std::vector<std::size_t> owner;
    for (auto c : cats)
        for (double v : space_.values(c)) {
            if (std::abs(v - best_.coords[c]) < 1e-9) continue;
            auto x = best_.coords;
            x[c] = v;
            probe.push_back(std::move(x));
            owner.push_back(c);
        }
    const auto got = evaluate(probe);
    std::vector<std::pair<double, std::size_t>> importance;
    for (auto c : cats) {
        double lo = best_.eval.loss, hi = best_.eval.loss;
        bool failed = false;
        for (std::size_t i = 0; i < got.size(); ++i) {
            if (owner[i] != c) continue;
            if (!got[i].eval.ok()) failed = true;
            else {
                lo = std::min(lo, got[i].eval.loss);
                hi = std::max(hi, got[i].eval.loss);
            }
        }
        importance.emplace_back(hi - lo + (failed ? 1.0 : 0.0), c);
    }
    std::stable_sort(importance.begin(), importance.end(), [](auto& a, auto& b) { return a.first > b.first; });
    std::vector<std::size_t> chosen;
    for (const auto& [imp, c] : importance)
        if (imp > 1e-6 && int(chosen.size()) < opt_.max_categorical) chosen.push_back(c);
    if (chosen.empty() || global_stop()) {
        end_stage();
        return;
    }
    // Enumerate every combination of the chosen parameters on the best point.
    std::vector<std::vector<double>> combos{best_.coords};
    for (auto c : chosen) {
        std::vector<std::vector<double>> next;
        for (const auto& base : combos)
            for (double v : space_.values(c)) {
                auto x = base;
                x[c] = v;
                next.push_back(std::move(x));
            }
        combos = std::move(next);
    }
    std::vector<std::vector<double>> others;
    for (auto& x : combos) {
        bool same = true;
        for (auto c : chosen) same = same && std::abs(x[c] - best_.coords[c]) < 1e-9;
        if (!same) others.push_back(std::move(x));
    }
    auto scored = evaluate(others);
    std::stable_sort(scored.begin(), scored.end(), [](const Candidate& a, const Candidate& b) { return a.fitness < b.fitness; });
    // Inner S5 for the two most promising other combinations (within 3 dB of the best [D]).
    const double margin = 3.0;
    int inner = 0;
    for (const auto& c : scored) {
        if (inner >= 2 || global_stop() || !std::isfinite(c.fitness) || c.fitness > best_.fitness + margin) break;
        const auto r = run_cma(std::format("S6 combo {}", inner + 1), space_.active(), c, opt_.sigma_polish,
                               share(0.35), opt_.seed + 9000u + std::uint64_t(inner));
        remember(r);
        ++inner;
    }
    end_stage();
}

void Runner::stage_refine() {
    const auto& active = space_.active();
    if (active.empty() || !std::isfinite(best_.eval.loss)) return;
    begin_stage("S7 refine", int(active.size()));
    const Budget b = share(1.0);
    for (double delta : {0.04, 0.01}) {
        if (stop(b)) break;
        std::vector<std::vector<double>> trials;
        std::vector<std::size_t> which;
        for (auto i : active) {
            const auto& s = space_.slot(i);
            const double step = s.steps >= 2 ? 1.0 / double(s.steps - 1) : delta;
            for (double sign : {-1.0, 1.0}) {
                auto x = best_.coords;
                x[i] = std::clamp(space_.snap(i, x[i] + sign * step), s.lo, s.hi);
                if (std::abs(x[i] - best_.coords[i]) < 1e-12) continue;
                trials.push_back(std::move(x));
                which.push_back(i);
            }
        }
        const double before = best_.fitness;
        const auto base = best_.coords;
        const auto got = evaluate(trials);
        // Combine every parameter's improving move; keep the combination if it beats the best single move.
        auto combined = base;
        bool any = false;
        for (auto i : active) {
            double bf = before;
            for (std::size_t t = 0; t < got.size(); ++t)
                if (which[t] == i && got[t].fitness < bf) {
                    bf = got[t].fitness;
                    combined[i] = got[t].coords[i];
                    any = true;
                }
        }
        if (any && !stop(b)) (void)evaluate({combined});
    }
    remember(best_);
    end_stage();
}

void Runner::select_top_k() {
    remember(best_);
    std::stable_sort(finalists_.begin(), finalists_.end(), [](const Candidate& a, const Candidate& b) { return a.fitness < b.fitness; });
    if (finalists_.size() > 24) finalists_.resize(24);
    std::vector<std::vector<double>> coords;
    for (const auto& f : finalists_) coords.push_back(f.coords);
    // Embeddings for the feature-space test: one cached render each, not counted as search evaluations;
    // a render that fails now is still recorded (rule 6).
    auto emb = obj_.embed(coords);
    for (std::size_t i = 0; i < finalists_.size() && i < emb.size(); ++i) {
        if (!emb[i].ok()) {
            ++res_.outcomes[emb[i].outcome];
            if (opt_.log) opt_.log(std::format("top-K: finalist {} failed to re-render ({})", i + 1, to_string(emb[i].outcome)));
            continue;
        }
        finalists_[i].eval.embedding = std::move(emb[i].embedding);
    }
    for (const auto& f : finalists_) {
        if (int(res_.top_k.size()) >= opt_.top_k) break;
        bool distinct = true;
        for (const auto& t : res_.top_k) {
            const bool near_params = space_.distance(t.coords, f.coords) < opt_.diversity_param;
            const bool same_sound = !f.eval.embedding.empty() && !t.eval.embedding.empty() &&
                                    embedding_distance(f.eval.embedding, t.eval.embedding) < opt_.diversity_feature;
            if (near_params || same_sound) distinct = false;
        }
        if (distinct) res_.top_k.push_back(f);
    }
}

PipelineResult Runner::run(const std::vector<std::vector<double>>& seeds) {
    best_.eval.loss = std::numeric_limits<double>::infinity();
    best_.fitness = std::numeric_limits<double>::infinity();
    best_.coords = space_.defaults();
    stage_ = "S0";
    // Defaults and the given seeds (fingerprint neighbours, warm starts) first, made points of this space:
    // fixed parameters (--fix, pinned pitch, ...) at default and steps on their grid.
    std::vector<std::vector<double>> first{space_.defaults()};
    for (const auto& s : seeds)
        if (s.size() == space_.size()) first.push_back(space_.conform(s));
    auto base = evaluate(first);
    for (std::size_t i = 1; i < base.size(); ++i)
        if (std::isfinite(base[i].fitness)) kept_.push_back(base[i]);

    if (enabled(1)) stage_seed();
    if (!space_.active().empty()) {
        // Every stage starts from the best so far; a stage only frees its group (spec 04 section 4).
        bool any_group = false;
        for (auto i : space_.active()) any_group = any_group || stage_of(space_.slot(i).group) < 5;
        if (any_group) stage_groups();
        if (enabled(5)) stage_polish();
    }
    if (enabled(6) && !global_stop()) stage_categorical();
    if (enabled(7) && !global_stop()) stage_refine();
    res_.time_capped = now() >= opt_.time_cap_s;
    select_top_k();
    res_.best = best_;
    res_.seconds = now();
    return std::move(res_);
}

}  // namespace

PipelineResult run_pipeline(Objective& objective, const Space& space, const PipelineOptions& options,
                            const std::vector<std::vector<double>>& seeds) {
    Runner runner(objective, space, options);
    return runner.run(seeds);
}

}  // namespace mimicry::opt
