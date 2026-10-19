#include "opt/Space.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string_view>

#include "host/ParamUtil.hpp"

namespace mimicry::opt {

namespace {

constexpr double kWarpRatio = 100.0;  // log warp from the probe: u = (r^x - 1) / (r - 1) [D]

std::string lower(std::string_view s) {
    std::string out(s);
    for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool listed(const std::vector<std::string>& list, const host::ParamDesc& p) {
    const std::string name = lower(p.name), id = std::to_string(p.id);
    for (const auto& item : list)
        if (lower(item) == name || item == id) return true;
    return false;
}

bool texts_numeric(const host::ParamDesc& p) {
    for (const auto& t : p.texts)
        for (const unsigned char c : t)
            if (std::isdigit(c)) return true;
    return false;
}

}  // namespace

int stage_of(ParamGroup g) noexcept {
    switch (g) {
        case ParamGroup::Osc:
        case ParamGroup::Amp: return 2;
        case ParamGroup::Filter:
        case ParamGroup::Env: return 3;
        case ParamGroup::Lfo:
        case ParamGroup::Fx: return 4;
        default: return 5;
    }
}

std::string_view to_string(Role role) noexcept {
    switch (role) {
        case Role::Free: return "free";
        case Role::Stepped: return "stepped";
        case Role::Categorical: return "categorical";
        case Role::Fixed: return "fixed";
    }
    return "fixed";
}

std::string_view to_string(Mode mode) noexcept {
    switch (mode) {
        case Mode::Hybrid: return "hybrid";
        case Mode::Guided: return "guided";
        case Mode::Unsupervised: return "unsupervised";
    }
    return "hybrid";
}

Space Space::build(const std::vector<host::ParamDesc>& params, host::PluginFormat /*format*/, const Profile* profile,
                   const SpaceOptions& options) {
    Space s;
    s.params_ = params;
    s.slots_.resize(params.size());
    s.defaults_.resize(params.size());
    for (std::size_t i = 0; i < params.size(); ++i) {
        const auto& p = params[i];
        Slot& slot = s.slots_[i];
        slot.index = i;
        slot.steps = p.steps;
        slot.names = classify_param(p.name, p.module, p.units);
        slot.group = slot.names.group;
        s.defaults_[i] = std::clamp(p.default_coord, 0.0, 1.0);
        const ParamProfile* prof = profile ? profile->find(p) : nullptr;
        if (prof) {
            slot.lo = std::clamp(prof->safe_lo, 0.0, 1.0);
            slot.hi = std::clamp(prof->safe_hi, slot.lo, 1.0);
            // Hybrid: a measured signature overrides the name guess (spec 04 section 2).
            if (options.mode == Mode::Hybrid && group_of(prof->measured) != ParamGroup::Unknown)
                slot.group = group_of(prof->measured);
            // Spec 04 section 1: log decode when the probe says the effect is uniform in log. For VST3 this
            // re-maps the plugin's own taper only when the probe measured it to be poor (02 section 1, plan
            // OQ9: clap-wrapper exposes a 20 Hz - 20 kHz cutoff linearly in its normalised value).
            slot.warp_log = prof->response == Response::Log && p.steps == 0 && !p.log_scale;
        }
        // The plugin's own texts show a linear taper over decades of Hz or ms: the exact log warp of that ratio,
        // probe or not (spec 04 section 9) [A].
        if (const double r = host::linear_wide_taper_ratio(p); r > 0) {
            slot.warp_log = true;
            slot.warp_ratio = std::min(r, 1e5);
        }
        if (p.steps >= 2 && p.steps <= options.max_categorical_values && (p.steps == 2 || !texts_numeric(p)) &&
            p.units.empty())
            slot.role = Role::Categorical;
        else if (p.steps >= 2)
            slot.role = Role::Stepped;
        else
            slot.role = Role::Free;

        auto fix = [&](const char* why) {
            if (slot.role == Role::Fixed) return;
            slot.role = Role::Fixed;
            slot.why = why;
        };
        if (p.flags.readonly) fix("read-only");
        if (p.flags.bypass) fix("bypass");
        if (p.flags.program_change) fix("program change");
        if (p.flags.hidden) fix("hidden");
        if (!p.flags.automatable) fix("not automatable");
        if (p.steps == 1) fix("single value");
        if (options.pin_seed && is_seed_param(p.name)) fix("seed (pinned)");
        if (options.skip_dead && prof && prof->dead()) fix("dead (probe)");
        if (options.pin_pitch && (prof ? prof->measured == ProbeClass::Pitch : is_pitch_param(p.name)))
            fix("pitch (applied to the notes)");
        if (!options.only.empty() && !listed(options.only, p)) fix("not in --only");
        if (listed(options.fix, p)) fix("--fix");
        if (options.mode == Mode::Unsupervised) slot.group = ParamGroup::Unknown;
    }
    // Pairs the probe found to interact are fitted in one stage (spec 04 section 3, step 5): the later
    // parameter of the pair joins the earlier one's group.
    if (profile && options.mode != Mode::Unsupervised) {
        auto index_of = [&](std::uint32_t id) -> long {
            for (std::size_t i = 0; i < params.size(); ++i)
                if (params[i].id == id && profile->find(params[i])) return long(i);
            return -1;
        };
        for (const auto& pr : profile->pairs) {
            if (pr.separable) continue;
            const long a = index_of(pr.a), b = index_of(pr.b);
            if (a < 0 || b < 0) continue;
            Slot& sa = s.slots_[std::size_t(a)];
            Slot& sb = s.slots_[std::size_t(b)];
            if (sa.role == Role::Fixed || sb.role == Role::Fixed) continue;
            const int ta = stage_of(sa.group), tb = stage_of(sb.group);
            if (ta >= 5 || tb >= 5 || ta == tb) continue;
            (ta < tb ? sb : sa).group = (ta < tb ? sa : sb).group;
        }
    }
    for (std::size_t i = 0; i < s.slots_.size(); ++i) {
        const Role r = s.slots_[i].role;
        if (r == Role::Free || r == Role::Stepped) s.active_.push_back(i);
        if (r == Role::Categorical) s.categorical_.push_back(i);
        if (s.level_ < 0 && r == Role::Free && is_output_level(params[i].name, params[i].module))
            s.level_ = static_cast<long>(i);
    }
    return s;
}

std::vector<double> Space::values(std::size_t index) const {
    const int steps = slots_[index].steps;
    std::vector<double> v;
    if (steps < 2) return v;
    for (int k = 0; k < steps; ++k) {
        const double u = double(k) / double(steps - 1);
        if (u >= slots_[index].lo - 1e-9 && u <= slots_[index].hi + 1e-9) v.push_back(u);
    }
    return v;
}

double snap_to_grid(int steps, double u) noexcept {
    return steps >= 2 ? std::round(u * (steps - 1)) / double(steps - 1) : u;
}

double Space::snap(std::size_t index, double u) const { return snap_to_grid(slots_[index].steps, u); }

double Space::to_u(std::size_t index, double x) const {
    const Slot& s = slots_[index];
    if (!s.warp_log) return x;
    return (std::pow(s.warp_ratio, x) - 1.0) / (s.warp_ratio - 1.0);
}

double Space::to_x(std::size_t index, double u) const {
    const Slot& s = slots_[index];
    if (!s.warp_log) return u;
    return std::log1p(std::max(u, 0.0) * (s.warp_ratio - 1.0)) / std::log(s.warp_ratio);
}

double Space::decode(const std::vector<std::size_t>& free, const std::vector<double>& x, std::vector<double>& coords) const {
    double penalty = 0;
    for (std::size_t k = 0; k < free.size() && k < x.size(); ++k) {
        const std::size_t i = free[k];
        const Slot& s = slots_[i];
        const double xlo = to_x(i, s.lo), xhi = to_x(i, s.hi);
        const double xc = std::clamp(x[k], xlo, xhi);
        penalty += (x[k] - xc) * (x[k] - xc);
        coords[i] = snap_inside(i, to_u(i, xc));
    }
    return penalty;
}

double Space::snap_inside(std::size_t index, double u) const {
    const Slot& s = slots_[index];
    u = snap(index, std::clamp(u, s.lo, s.hi));
    if (s.steps >= 2) {
        const double step = 1.0 / double(s.steps - 1);
        if (u > s.hi + 1e-12) u -= step;
        if (u < s.lo - 1e-12) u += step;
    }
    return std::clamp(u, 0.0, 1.0);
}

std::vector<double> Space::encode(const std::vector<std::size_t>& free, const std::vector<double>& coords) const {
    std::vector<double> x(free.size());
    for (std::size_t k = 0; k < free.size(); ++k) x[k] = to_x(free[k], coords[free[k]]);
    return x;
}

std::vector<double> Space::sample_box(const std::vector<double>& unit, Rng& rng) const {
    auto c = defaults_;
    std::vector<double> x(active_.size());
    for (std::size_t k = 0; k < active_.size() && k < unit.size(); ++k) {
        const Slot& s = slots_[active_[k]];
        const double lo = to_x(active_[k], s.lo), hi = to_x(active_[k], s.hi);
        x[k] = lo + unit[k] * (hi - lo);
    }
    decode(active_, x, c);
    for (auto ci : categorical_) {
        const auto vals = values(ci);
        if (!vals.empty()) c[ci] = vals[rng.below(vals.size())];
    }
    return c;
}

std::vector<double> Space::conform(std::vector<double> coords) const {
    coords.resize(slots_.size());
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        if (slots_[i].role == Role::Fixed || !std::isfinite(coords[i])) coords[i] = defaults_[i];
        else coords[i] = snap_inside(i, coords[i]);
    }
    return coords;
}

double Space::prior(const std::vector<double>& coords) const {
    if (active_.empty()) return 0;
    double s = 0;
    for (auto i : active_) s += (coords[i] - defaults_[i]) * (coords[i] - defaults_[i]);
    return s / double(active_.size());
}

double Space::distance(const std::vector<double>& a, const std::vector<double>& b) const {
    double s = 0;
    std::size_t n = 0;
    for (auto list : {&active_, &categorical_})
        for (auto i : *list) {
            s += (a[i] - b[i]) * (a[i] - b[i]);
            ++n;
        }
    return n ? std::sqrt(s / double(n)) : 0.0;
}

}  // namespace mimicry::opt
