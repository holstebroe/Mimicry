#pragma once
// Learned hazard constraints (docs/spec/05_RESILIENCE.md section 4): when a candidate fails and a known good
// vector exists, delta debugging (ddmin) finds the smallest set of parameters that reproduces the failure,
// a bisection per parameter finds the boundary, and the region (a box in u) is stored in the plugin profile.
// Later candidates inside a region are redrawn before they are rendered (spec 05 section 3).
//
// A constraint keys its parameters on id AND name (CLAUDE.md rule 7); `index` is resolved per run against
// the plugin's parameter table and never stored.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <vector>

#include "core/Expected.hpp"
#include "core/Json.hpp"
#include "core/Outcome.hpp"
#include "host/PluginHost.hpp"

namespace mimicry::opt {

struct ConstraintBound {
    std::uint32_t id = 0;
    std::string name;
    std::size_t index = 0;  // into the parameter table of this run (not stored)
    double lo = 0, hi = 1;  // the failing interval in u
    double witness = 0;     // the failing value that was bisected (stays inside when the box shrinks)
};

struct Constraint {
    std::vector<ConstraintBound> bounds;  // the minimal failing set S, one box side per parameter
    Outcome outcome = Outcome::Crash;
    int count = 1;         // failures seen inside the region (the bisection's own counts once)
    int contradicted = 0;  // valid renders seen inside it (each shrank the box)

    // Every bounded parameter of `coords` (a full vector) inside its interval.
    [[nodiscard]] bool contains(const std::vector<double>& coords) const;
    // count / (count + contradicted + 1): grows with repeats, falls with contradictions.
    [[nodiscard]] double confidence() const noexcept;
    // "Hazard A >= 0.801 && Hazard B >= 0.902 -> crash".
    [[nodiscard]] std::string describe() const;
};

// Renders full vectors and returns only their outcomes, in order (Objective::outcomes).
using OutcomeBatch = std::function<std::vector<Outcome>(const std::vector<std::vector<double>>&)>;

struct BisectOptions {
    double tolerance = 0.004;  // threshold interval width in u [D] (T8 needs 2 %)
    int points = 3;            // interior points per parameter per round: the interval shrinks by points+1 [D]
    int max_renders = 160;     // per failure [D]
    double min_delta = 1e-9;   // parameters closer than this to the good vector are not part of the search
};

struct BisectResult {
    bool reproduced = false;  // the failing vector failed again with the same outcome, the good one passed
    std::vector<std::size_t> minimal;  // S: parameter indices
    std::vector<double> threshold;     // per entry of `minimal`: boundary between good and failing (in u)
    long renders = 0;
};

// Delta debugging + threshold bisection between a good and a failing full vector (spec 05 section 4).
// A probe vector "fails" when it renders with `outcome`. Every render goes through `test`, so a cached
// objective makes repeated vectors free.
[[nodiscard]] BisectResult bisect_failure(const OutcomeBatch& test, const std::vector<double>& good,
                                          const std::vector<double>& bad, Outcome outcome,
                                          const BisectOptions& options = {});

// The region of a bisection: for each parameter of S, from the threshold to the edge of the unit range on
// the failing side [D] (the box is extended to the edge because nothing past the failing value was rendered;
// a valid render inside shrinks it).
[[nodiscard]] Constraint make_constraint(const BisectResult& result, const std::vector<double>& bad, Outcome outcome,
                                         const std::vector<host::ParamDesc>& params);

class ConstraintSet {
public:
    ConstraintSet() = default;
    explicit ConstraintSet(std::vector<host::ParamDesc> params) : params_(std::move(params)) {}

    // Takes stored constraints (a profile's); those whose parameters are not all in this table with the
    // same id and name are skipped (rule 7). Returns how many were taken.
    std::size_t adopt(const std::vector<Constraint>& stored);
    // Adds a learned constraint, merged into an existing one over the same parameters whose box overlaps.
    void add(Constraint c);

    // The first constraint containing `coords`, or null (the geometric test of spec 05 section 4).
    [[nodiscard]] const Constraint* find(const std::vector<double>& coords) const;
    // A failure at `coords`: counted by every constraint that contains it with the same outcome; false
    // when none does (then it is new information worth a bisection).
    bool observe_failure(const std::vector<double>& coords, Outcome outcome);
    // A valid render at `coords` contradicts every constraint containing it: the box shrinks just past the
    // point on the side that loses least while keeping its witness inside, else the constraint is dropped.
    // Returns the number of constraints changed.
    int observe_valid(const std::vector<double>& coords);

    [[nodiscard]] const std::vector<Constraint>& constraints() const noexcept { return list_; }
    [[nodiscard]] bool empty() const noexcept { return list_.empty(); }
    [[nodiscard]] int dropped() const noexcept { return dropped_; }

    [[nodiscard]] static json::Value to_json(const std::vector<Constraint>& list);
    [[nodiscard]] static expected<std::vector<Constraint>, std::string> from_json(const json::Value& v);

private:
    std::vector<host::ParamDesc> params_;
    std::vector<Constraint> list_;
    int dropped_ = 0;
};

}  // namespace mimicry::opt
