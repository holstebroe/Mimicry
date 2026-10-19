#include "opt/Constraints.hpp"

#include <algorithm>
#include <cmath>
#include <format>

namespace mimicry::opt {

namespace {

constexpr double kEdge = 1e-6;  // a shrunk box stops this far past the valid point

// The vector `good` with the parameters in `set` taken from `bad`.
std::vector<double> with(const std::vector<double>& good, const std::vector<double>& bad, const std::vector<std::size_t>& set) {
    auto v = good;
    for (auto i : set) v[i] = bad[i];
    return v;
}

// `c` in `n` contiguous, nearly equal parts.
std::vector<std::vector<std::size_t>> split(const std::vector<std::size_t>& c, std::size_t n) {
    std::vector<std::vector<std::size_t>> parts(n);
    for (std::size_t k = 0; k < n; ++k)
        parts[k].assign(c.begin() + long(k * c.size() / n), c.begin() + long((k + 1) * c.size() / n));
    return parts;
}

std::vector<std::size_t> without(const std::vector<std::size_t>& c, const std::vector<std::size_t>& part) {
    std::vector<std::size_t> out;
    for (auto i : c)
        if (std::find(part.begin(), part.end(), i) == part.end()) out.push_back(i);
    return out;
}

bool same_params(const Constraint& a, const Constraint& b) {
    if (a.bounds.size() != b.bounds.size()) return false;
    for (const auto& x : a.bounds) {
        bool found = false;
        for (const auto& y : b.bounds) found = found || (x.id == y.id && x.name == y.name);
        if (!found) return false;
    }
    return true;
}

const ConstraintBound* bound_of(const Constraint& c, std::uint32_t id, const std::string& name) {
    for (const auto& b : c.bounds)
        if (b.id == id && b.name == name) return &b;
    return nullptr;
}

}  // namespace

bool Constraint::contains(const std::vector<double>& coords) const {
    if (bounds.empty()) return false;
    for (const auto& b : bounds) {
        if (b.index >= coords.size()) return false;
        const double u = coords[b.index];
        if (u < b.lo || u > b.hi) return false;
    }
    return true;
}

double Constraint::confidence() const noexcept {
    return double(count) / (double(count) + double(contradicted) + 1.0);
}

std::string Constraint::describe() const {
    std::string s;
    for (const auto& b : bounds) {
        if (!s.empty()) s += " && ";
        if (b.lo <= 0 && b.hi >= 1) s += std::format("{} any", b.name);
        else if (b.hi >= 1) s += std::format("{} >= {:.3f}", b.name, b.lo);
        else if (b.lo <= 0) s += std::format("{} <= {:.3f}", b.name, b.hi);
        else s += std::format("{} in [{:.3f}, {:.3f}]", b.name, b.lo, b.hi);
    }
    return s + " -> " + std::string(to_string(outcome));
}

BisectResult bisect_failure(const OutcomeBatch& test, const std::vector<double>& good, const std::vector<double>& bad,
                            Outcome outcome, const BisectOptions& options) {
    BisectResult r;
    if (good.size() != bad.size() || outcome == Outcome::Ok) return r;
    auto run = [&](const std::vector<std::vector<double>>& vs) {
        r.renders += long(vs.size());
        auto got = test(vs);
        got.resize(vs.size(), Outcome::HostError);
        return got;
    };
    const auto first = run({good, bad});
    if (first[0] != Outcome::Ok || first[1] != outcome) return r;
    r.reproduced = true;

    std::vector<std::size_t> c;
    for (std::size_t i = 0; i < good.size(); ++i)
        if (std::abs(bad[i] - good[i]) > options.min_delta) c.push_back(i);

    // 1. ddmin (Zeller): test the n parts and, for n > 2, their complements in one batch; keep the first
    //    failing one, else double the granularity until it equals |c|.
    std::size_t n = 2;
    while (c.size() >= 2 && r.renders < options.max_renders) {
        n = std::min(n, c.size());
        const auto parts = split(c, n);
        std::vector<std::vector<std::size_t>> sets = parts;
        if (n > 2)
            for (const auto& p : parts) sets.push_back(without(c, p));
        std::vector<std::vector<double>> vs;
        for (const auto& s : sets) vs.push_back(with(good, bad, s));
        const auto got = run(vs);
        std::size_t hit = sets.size();
        for (std::size_t k = 0; k < sets.size() && hit == sets.size(); ++k)
            if (got[k] == outcome) hit = k;
        if (hit < n) {
            c = sets[hit];
            n = 2;
        } else if (hit < sets.size()) {
            c = sets[hit];
            n = std::max<std::size_t>(n - 1, 2);
        } else if (n >= c.size()) {
            break;
        } else {
            n = std::min(2 * n, c.size());
        }
    }
    r.minimal = c;

    // 2. Thresholds: each parameter of S moves from its good value towards its failing value with the rest
    //    of S at their failing values and everything else good; `points` interior points per round.
    const auto background = with(good, bad, c);
    std::vector<double> lo(c.size()), hi(c.size());  // lo passes, hi fails
    for (std::size_t k = 0; k < c.size(); ++k) {
        lo[k] = good[c[k]];
        hi[k] = bad[c[k]];
    }
    const int pts = std::max(1, options.points);
    while (r.renders < options.max_renders) {
        std::vector<std::vector<double>> vs;
        std::vector<std::size_t> owner;
        for (std::size_t k = 0; k < c.size(); ++k) {
            if (std::abs(hi[k] - lo[k]) <= options.tolerance) continue;
            for (int j = 1; j <= pts; ++j) {
                auto v = background;
                v[c[k]] = lo[k] + (hi[k] - lo[k]) * double(j) / double(pts + 1);
                vs.push_back(std::move(v));
                owner.push_back(k);
            }
        }
        if (vs.empty()) break;
        const auto got = run(vs);
        for (std::size_t k = 0; k < c.size(); ++k) {
            // The first failing point along good -> bad sets the new failing end, the point before it the
            // passing end (failure assumed monotone along the segment).
            double new_lo = lo[k], new_hi = hi[k];
            for (std::size_t t = 0; t < vs.size(); ++t) {
                if (owner[t] != k) continue;
                const double u = vs[t][c[k]];
                if (got[t] == outcome) {
                    new_hi = u;
                    break;
                }
                new_lo = u;
            }
            lo[k] = new_lo;
            hi[k] = new_hi;
        }
    }
    for (std::size_t k = 0; k < c.size(); ++k) r.threshold.push_back(0.5 * (lo[k] + hi[k]));
    return r;
}

Constraint make_constraint(const BisectResult& result, const std::vector<double>& bad, Outcome outcome,
                           const std::vector<host::ParamDesc>& params) {
    Constraint c;
    c.outcome = outcome;
    for (std::size_t k = 0; k < result.minimal.size() && k < result.threshold.size(); ++k) {
        const std::size_t i = result.minimal[k];
        ConstraintBound b;
        b.index = i;
        if (i < params.size()) {
            b.id = params[i].id;
            b.name = params[i].name;
        }
        b.witness = bad[i];
        const double t = std::clamp(result.threshold[k], 0.0, 1.0);
        if (bad[i] >= t) {
            b.lo = t;
            b.hi = 1;
        } else {
            b.lo = 0;
            b.hi = t;
        }
        c.bounds.push_back(std::move(b));
    }
    return c;
}

std::size_t ConstraintSet::adopt(const std::vector<Constraint>& stored) {
    std::size_t taken = 0;
    for (auto c : stored) {
        bool ok = !c.bounds.empty();
        for (auto& b : c.bounds) {
            bool found = false;
            for (std::size_t i = 0; i < params_.size() && !found; ++i)
                if (params_[i].id == b.id && params_[i].name == b.name) {
                    b.index = i;
                    found = true;
                }
            ok = ok && found;
        }
        if (!ok) continue;
        add(std::move(c));
        ++taken;
    }
    return taken;
}

void ConstraintSet::add(Constraint c) {
    for (auto& e : list_) {
        if (e.outcome != c.outcome || !same_params(e, c)) continue;
        bool overlap = true;
        for (const auto& b : c.bounds) {
            const auto* eb = bound_of(e, b.id, b.name);
            overlap = overlap && eb && b.lo <= eb->hi && eb->lo <= b.hi;
        }
        if (!overlap) continue;
        for (auto& eb : e.bounds) {
            const auto* b = bound_of(c, eb.id, eb.name);
            eb.lo = std::min(eb.lo, b->lo);
            eb.hi = std::max(eb.hi, b->hi);
        }
        e.count += c.count;
        e.contradicted += c.contradicted;
        return;
    }
    list_.push_back(std::move(c));
}

const Constraint* ConstraintSet::find(const std::vector<double>& coords) const {
    for (const auto& c : list_)
        if (c.contains(coords)) return &c;
    return nullptr;
}

bool ConstraintSet::observe_failure(const std::vector<double>& coords, Outcome outcome) {
    bool known = false;
    for (auto& c : list_)
        if (c.outcome == outcome && c.contains(coords)) {
            ++c.count;
            known = true;
        }
    return known;
}

int ConstraintSet::observe_valid(const std::vector<double>& coords) {
    int changed = 0;
    for (auto it = list_.begin(); it != list_.end();) {
        Constraint& c = *it;
        if (!c.contains(coords)) {
            ++it;
            continue;
        }
        ++changed;
        ++c.contradicted;
        // Move the one box side that loses the smallest share of its interval past the valid point.
        ConstraintBound* pick = nullptr;
        double best = std::numeric_limits<double>::infinity(), to = 0;
        bool raise = false;
        for (auto& b : c.bounds) {
            const double u = coords[b.index], width = std::max(b.hi - b.lo, 1e-12);
            if (b.witness > u + kEdge && (u + kEdge - b.lo) / width < best) {
                best = (u + kEdge - b.lo) / width;
                pick = &b;
                to = u + kEdge;
                raise = true;
            }
            if (b.witness < u - kEdge && (b.hi - (u - kEdge)) / width < best) {
                best = (b.hi - (u - kEdge)) / width;
                pick = &b;
                to = u - kEdge;
                raise = false;
            }
        }
        // Dropped when the failing point itself rendered valid, or when contradictions outnumber the
        // failures seen inside by three [D].
        if (!pick || c.contradicted >= c.count + 3) {
            it = list_.erase(it);
            ++dropped_;
            continue;
        }
        (raise ? pick->lo : pick->hi) = to;
        ++it;
    }
    return changed;
}

json::Value ConstraintSet::to_json(const std::vector<Constraint>& list) {
    json::Array out;
    for (const auto& c : list) {
        json::Value e;
        e.set("outcome", std::string(to_string(c.outcome)));
        e.set("count", c.count);
        e.set("contradicted", c.contradicted);
        e.set("confidence", c.confidence());
        json::Array ps;
        for (const auto& b : c.bounds) {
            json::Value p;
            p.set("id", b.id);
            p.set("name", b.name);
            p.set("lo", b.lo);
            p.set("hi", b.hi);
            p.set("witness", b.witness);
            ps.push_back(std::move(p));
        }
        e.set("params", json::Value(std::move(ps)));
        out.push_back(std::move(e));
    }
    return json::Value(std::move(out));
}

expected<std::vector<Constraint>, std::string> ConstraintSet::from_json(const json::Value& v) {
    if (!v.is_null() && !v.is_array()) return unexpected(std::string("constraints: not an array"));
    std::vector<Constraint> out;
    for (const auto& e : v.as_array()) {
        Constraint c;
        c.outcome = parse_outcome(e["outcome"].as_string());
        c.count = int(e["count"].as_number(1));
        c.contradicted = int(e["contradicted"].as_number(0));
        for (const auto& p : e["params"].as_array()) {
            ConstraintBound b;
            b.id = static_cast<std::uint32_t>(p["id"].as_number());
            b.name = p["name"].as_string();
            b.lo = p["lo"].as_number(0);
            b.hi = p["hi"].as_number(1);
            b.witness = p["witness"].as_number(0.5 * (b.lo + b.hi));
            c.bounds.push_back(std::move(b));
        }
        if (c.outcome == Outcome::Ok || c.bounds.empty())
            return unexpected(std::string("constraints: an entry without parameters or outcome"));
        out.push_back(std::move(c));
    }
    return out;
}

}  // namespace mimicry::opt
