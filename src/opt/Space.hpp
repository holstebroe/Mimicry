#pragma once
// The search space (docs/spec/04_OPTIMIZER.md section 1): which parameters are searched, how, and the
// decode from the strategy's coordinates to a full render vector.
//
// The render coordinate `u` is the host's (02 section 1): the plugin's own normalised value for VST3, the
// plain value mapped linear or log for CLAP (the backend applies that map). On top of it:
//   - stepped parameters are carried continuous and rounded to their grid at decode;
//   - enums and booleans are categorical: never searched by CMA-ES, enumerated by the outer loop (S6);
//   - a CLAP parameter the probe found to act uniformly in log gets a log warp in the strategy [D];
//   - decode clips to the safe box and reports the clip distance for the boundary penalty.
// Fixed: read-only, bypass, program-change, hidden, seed/random (pinned), dead (probe), --fix, and
// everything outside --only.

#include <cstddef>
#include <string>
#include <vector>

#include "core/ParamSemantics.hpp"
#include "core/Rng.hpp"
#include "host/PluginHost.hpp"
#include "opt/Profile.hpp"

namespace mimicry::opt {

enum class Role { Free, Stepped, Categorical, Fixed };
[[nodiscard]] std::string_view to_string(Role role) noexcept;

enum class Mode { Hybrid, Guided, Unsupervised };
[[nodiscard]] std::string_view to_string(Mode mode) noexcept;

// The stage that frees a group (spec 04 section 4): 2 osc + amp, 3 filter + env, 4 lfo + fx, 5 = only the
// joint polish (global, unknown).
[[nodiscard]] int stage_of(ParamGroup group) noexcept;

// Rounds u to the grid of a parameter with `steps` values (steps < 2: continuous, unchanged).
[[nodiscard]] double snap_to_grid(int steps, double u) noexcept;

struct Slot {
    std::size_t index = 0;   // into Instance::parameters()
    Role role = Role::Free;
    ParamGroup group = ParamGroup::Unknown;  // stage group: names, or the probe in hybrid mode
    Semantics names;
    int steps = 0;           // > 1 for stepped and categorical
    double lo = 0, hi = 1;   // safe box in u
    bool warp_log = false;
    double warp_ratio = 100;  // r of the log warp u = (r^x - 1) / (r - 1)
    std::string why;         // reason a parameter is fixed
};

struct SpaceOptions {
    Mode mode = Mode::Hybrid;
    std::vector<std::string> only;     // names (case-insensitive) or ids; empty = all
    std::vector<std::string> fix;      // names or ids pinned at their default
    int max_categorical_values = 16;   // more values than this: stepped, not categorical [D]
    bool pin_seed = true;              // spec 03 section 6
    bool skip_dead = true;             // probe: dead parameters frozen at default
    // S0 applies the target's pitch to the notes (key + cents), so parameters that only shift pitch are
    // pinned: the probe's `pitch` class, or without a profile a name like tune / fine / pitch [D]. A pitch
    // basin is a few cents wide and flat around it, which no spectral loss can search.
    bool pin_pitch = false;
};

class Space {
public:
    Space() = default;
    // `profile` may be null (no probe yet); it must belong to the same plugin.
    [[nodiscard]] static Space build(const std::vector<host::ParamDesc>& params, host::PluginFormat format,
                                     const Profile* profile, const SpaceOptions& options);

    [[nodiscard]] std::size_t size() const noexcept { return slots_.size(); }
    [[nodiscard]] const std::vector<Slot>& slots() const noexcept { return slots_; }
    [[nodiscard]] const Slot& slot(std::size_t i) const { return slots_[i]; }
    [[nodiscard]] const std::vector<host::ParamDesc>& params() const noexcept { return params_; }
    [[nodiscard]] const std::vector<double>& defaults() const noexcept { return defaults_; }

    // Parameter indices of the searched (free + stepped) and categorical parameters.
    [[nodiscard]] const std::vector<std::size_t>& active() const noexcept { return active_; }
    [[nodiscard]] const std::vector<std::size_t>& categorical() const noexcept { return categorical_; }

    // The coordinates a categorical (or stepped) parameter can take.
    [[nodiscard]] std::vector<double> values(std::size_t index) const;
    // Rounds a stepped or categorical u to its grid; continuous u unchanged.
    [[nodiscard]] double snap(std::size_t index, double u) const;

    // Strategy coordinate x <-> render coordinate u for one parameter (warp only).
    [[nodiscard]] double to_u(std::size_t index, double x) const;
    [[nodiscard]] double to_x(std::size_t index, double u) const;

    // Writes the strategy vector `x` (one entry per `free`) into `coords` (a full vector), clipping to the
    // safe box and snapping steps. Returns the sum of squared clip distances (boundary penalty input).
    double decode(const std::vector<std::size_t>& free, const std::vector<double>& x, std::vector<double>& coords) const;
    // The strategy vector for `free` read from a full vector.
    [[nodiscard]] std::vector<double> encode(const std::vector<std::size_t>& free, const std::vector<double>& coords) const;

    // A full vector from one unit-cube point (one entry per active parameter, e.g. a Latin-hypercube row):
    // the active parameters spread over the safe box in x, categoricals drawn uniformly from `rng`, the
    // rest at default (S1 and the fingerprint presets).
    [[nodiscard]] std::vector<double> sample_box(const std::vector<double>& unit, Rng& rng) const;
    // A vector from outside the search (a fingerprint row, a warm start) made a point of this space: fixed
    // parameters at their defaults, the others clipped to the safe box and snapped to their grid.
    [[nodiscard]] std::vector<double> conform(std::vector<double> coords) const;

    // Mean squared distance from the defaults over the active parameters, in u (the prior, spec 04 section 6).
    [[nodiscard]] double prior(const std::vector<double>& coords) const;
    // RMS distance over the active and categorical parameters, in u (diversity, spec 04 section 5).
    [[nodiscard]] double distance(const std::vector<double>& a, const std::vector<double>& b) const;

    // Index of the output level parameter (spec 03 section 4) or -1.
    [[nodiscard]] long level_index() const noexcept { return level_; }

private:
    // snap() inside the safe box: a grid point just outside an off-grid bound moves one step inwards.
    [[nodiscard]] double snap_inside(std::size_t index, double u) const;

    std::vector<host::ParamDesc> params_;
    std::vector<Slot> slots_;
    std::vector<double> defaults_;
    std::vector<std::size_t> active_, categorical_;
    long level_ = -1;
};

}  // namespace mimicry::opt
