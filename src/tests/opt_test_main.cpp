// Tests for WP3 that need no plugin: CMA-ES and sep-CMA-ES on known functions (T5, docs/spec/
// 07_OUTPUT_VALIDATION.md), the Jacobi eigensolver, sampling, semantics, the encoding, the surrogate, the
// profile and fingerprint files, the embedding, failure bisection and the constraint store (T8 without a
// plugin), and the pipeline on a synthetic objective with a planted crash region.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <format>
#include <numbers>
#include <string>
#include <thread>
#include <vector>

#include "core/Cma.hpp"
#include "core/ParamSemantics.hpp"
#include "core/Rng.hpp"
#include "core/Sampling.hpp"
#include "opt/Constraints.hpp"
#include "opt/Embedding.hpp"
#include "opt/Fingerprint.hpp"
#include "opt/Pipeline.hpp"
#include "opt/Probe.hpp"
#include "opt/Profile.hpp"
#include "opt/Space.hpp"
#include "opt/Surrogate.hpp"

using namespace mimicry;
using namespace mimicry::opt;

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
        ++failures;
    }
}

double sphere(const std::vector<double>& x) {
    double s = 0;
    for (double v : x) s += v * v;
    return s;
}

double rosenbrock(const std::vector<double>& x) {
    double s = 0;
    for (std::size_t i = 0; i + 1 < x.size(); ++i)
        s += 100 * (x[i + 1] - x[i] * x[i]) * (x[i + 1] - x[i] * x[i]) + (1 - x[i]) * (1 - x[i]);
    return s;
}

double rastrigin(const std::vector<double>& x) {
    double s = 10.0 * double(x.size());
    for (double v : x) s += v * v - 10 * std::cos(2 * std::numbers::pi * v);
    return s;
}

double ellipsoid(const std::vector<double>& x) {
    double s = 0;
    const double n = double(x.size());
    for (std::size_t i = 0; i < x.size(); ++i) s += std::pow(1e3, double(i) / (n - 1)) * x[i] * x[i];
    return s;
}

void test_jacobi() {
    Rng rng(3);
    const int n = 7;
    std::vector<double> a(n * n);
    for (int i = 0; i < n; ++i)
        for (int j = 0; j <= i; ++j) a[std::size_t(i * n + j)] = a[std::size_t(j * n + i)] = rng.uniform(-1, 1);
    std::vector<double> val, vec;
    jacobi_eigen(a, n, val, vec);
    double err = 0;
    for (int k = 0; k < n; ++k)
        for (int i = 0; i < n; ++i) {
            double av = 0;
            for (int j = 0; j < n; ++j) av += a[std::size_t(i * n + j)] * vec[std::size_t(j * n + k)];
            err = std::max(err, std::abs(av - val[std::size_t(k)] * vec[std::size_t(i * n + k)]));
        }
    check(err < 1e-9, "Jacobi: A v = lambda v (err " + std::to_string(err) + ")");
}

void test_cma() {
    CmaRunOptions o;
    o.cma.sigma0 = 0.5;
    o.sigma_min = 1e-14;
    o.target = 1e-10;
    o.max_evals = 20000;
    auto r = cma_minimize(sphere, std::vector<double>(10, 1.0), o);
    check(r.f < 1e-10, "T5 CMA-ES sphere 10D (f " + std::to_string(r.f) + ")");

    o.max_evals = 60000;
    r = cma_minimize(rosenbrock, std::vector<double>(8, 0.0), o);
    check(r.f < 1e-8, "T5 CMA-ES Rosenbrock 8D (f " + std::to_string(r.f) + ")");

    // Rastrigin: restarts with a doubled population, then independent runs from different seeds (R).
    double best = 1e300;
    for (std::uint64_t seed = 1; seed <= 6 && best > 1e-8; ++seed) {
        CmaRunOptions ro;
        ro.cma.sigma0 = 2.0;
        ro.cma.seed = seed * 101;
        ro.cma.lambda = 40;
        ro.max_lambda = 320;
        ro.restart_sigma = 1.0;
        ro.sigma_min = 1e-10;
        ro.target = 1e-10;
        ro.max_evals = 80000;
        Rng start(seed);
        std::vector<double> x0(5);
        for (auto& v : x0) v = start.uniform(-4, 4);
        best = std::min(best, cma_minimize(rastrigin, x0, ro).f);
    }
    check(best < 1e-8, "T5 CMA-ES Rastrigin 5D with restarts (f " + std::to_string(best) + ")");

    CmaRunOptions so;
    so.cma.sigma0 = 0.5;
    so.cma.separable = true;
    so.sigma_min = 1e-14;
    so.target = 1e-8;
    so.max_evals = 200000;
    r = cma_minimize(ellipsoid, std::vector<double>(100, 1.0), so);
    check(r.f < 1e-8, "T5 sep-CMA-ES ellipsoid 100D (f " + std::to_string(r.f) + ")");
    so.max_evals = 100000;
    r = cma_minimize(sphere, std::vector<double>(200, 1.0), so);
    check(r.f < 1e-8, "T5 sep-CMA-ES sphere 200D (f " + std::to_string(r.f) + ")");

    // tell() accepts any candidate set (pre-screened subsets).
    Cma cma(std::vector<double>(3, 0.5), CmaOptions{});
    auto xs = cma.ask(12);
    std::vector<double> f(xs.size());
    for (std::size_t i = 0; i < xs.size(); ++i) f[i] = sphere(xs[i]);
    cma.tell({xs.begin(), xs.begin() + 5}, {f.begin(), f.begin() + 5});
    check(cma.generation() == 1 && std::isfinite(cma.sigma()), "CMA-ES tell with a subset");

    // The restart policy the pipeline shares: run 0 as given, then sigma halved, population doubled, cap 64.
    CmaRunOptions rp;
    rp.cma.sigma0 = 0.3;
    rp.cma.seed = 5;
    const auto r0 = cma_restart_options(rp, 0, 0), r1 = cma_restart_options(rp, 1, 10), r2 = cma_restart_options(rp, 2, 40);
    check(r0.sigma0 == 0.3 && r0.lambda == 0 && r0.seed == 5, "CMA-ES restart 0 keeps the options");
    check(std::abs(r1.sigma0 - 0.15) < 1e-15 && r1.lambda == 20 && r1.seed == 5 + 7919u && r2.lambda == 64,
          "CMA-ES restart: sigma halved, population doubled up to 64");
}

void test_sampling() {
    Rng rng(9);
    const auto pts = latin_hypercube(50, 4, rng);
    bool ok = pts.size() == 50;
    for (std::size_t d = 0; d < 4; ++d) {
        std::vector<int> hit(50, 0);
        for (const auto& p : pts) {
            ok = ok && p[d] >= 0 && p[d] < 1;
            ++hit[std::size_t(p[d] * 50)];
        }
        for (int h : hit) ok = ok && h == 1;
    }
    check(ok, "LHS: one point per stratum in every dimension");
}

void test_semantics() {
    auto g = [](const char* name, const char* module = "", const char* units = "") {
        return classify_param(name, module, units).group;
    };
    check(g("Filter Cutoff") == ParamGroup::Filter, "semantics: cutoff");
    check(g("Resonance") == ParamGroup::Filter, "semantics: resonance");
    check(g("LFO Rate") == ParamGroup::Lfo && g("LFO1 Freq") == ParamGroup::Lfo, "semantics: lfo beats freq");
    check(g("Attack", "Amp Env") == ParamGroup::Env, "semantics: attack");
    check(g("Osc2Wave") == ParamGroup::Osc && g("Detune") == ParamGroup::Osc, "semantics: osc");
    check(g("Reverb Mix") == ParamGroup::Fx, "semantics: fx");
    check(g("Master Volume") == ParamGroup::Amp, "semantics: amp");
    check(g("Glide") == ParamGroup::Global, "semantics: global");
    check(g("Foo", "", "Hz") == ParamGroup::Filter && g("Bar") == ParamGroup::Unknown, "semantics: units hint, unknown");
    check(classify_param("Cutoff", "Filter", "Hz").confidence > classify_param("Foo", "", "Hz").confidence, "semantics: confidence");
    check(is_output_level("Level", "Amp") && is_output_level("Master Volume", "") && !is_output_level("Osc 1 Level", "") &&
              !is_output_level("Level", "Osc"), "output level detection");
    check(is_pitch_param("Fine Tune") && is_pitch_param("Osc Pitch") && !is_pitch_param("Pitch Env Amount") &&
              !is_pitch_param("Detune"), "pitch parameter names");
    check(is_seed_param("LFO Seed") && is_seed_param("Random Phase") && !is_seed_param("Cutoff"), "seed detection");
    check(parse_group("filter") == ParamGroup::Filter && to_string(ParamGroup::Env) == "env", "group names");
}

host::ParamDesc param(std::uint32_t id, std::string name, int steps = 0, double def = 0.5, std::string units = "",
                      std::array<std::string, 5> texts = {}) {
    host::ParamDesc p;
    p.id = id;
    p.name = std::move(name);
    p.steps = steps;
    p.default_coord = def;
    p.units = std::move(units);
    p.texts = std::move(texts);
    return p;
}

std::vector<host::ParamDesc> synthetic_params() {
    std::vector<host::ParamDesc> ps;
    ps.push_back(param(10, "Osc Pitch"));
    ps.push_back(param(11, "Cutoff", 0, 0.7, "Hz"));
    ps.push_back(param(12, "Attack", 0, 0.1, "ms"));
    ps.push_back(param(13, "Decay", 0, 0.3, "ms"));
    ps.push_back(param(14, "LFO Depth", 0, 0.0));
    ps.push_back(param(15, "Reverb Mix", 0, 0.2));
    ps.push_back(param(16, "Octave", 5, 0.5, "", {"-2", "-1", "0", "1", "2"}));
    ps.push_back(param(17, "Wave", 4, 0.0, "", {"Sine", "Saw", "Square", "Noise", "Noise"}));
    ps.push_back(param(18, "Sync", 2, 0.0, "", {"Off", "Off", "On", "On", "On"}));
    auto meter = param(19, "Meter");
    meter.flags.readonly = true;
    ps.push_back(meter);
    ps.push_back(param(20, "Noise Seed", 256, 0.0));
    auto prog = param(21, "Program", 8, 0.0);
    prog.flags.program_change = true;
    ps.push_back(prog);
    return ps;
}

void test_space() {
    const auto ps = synthetic_params();
    SpaceOptions so;
    auto s = Space::build(ps, host::PluginFormat::Clap, nullptr, so);
    check(s.slot(0).role == Role::Free && s.slot(6).role == Role::Stepped && s.slot(7).role == Role::Categorical &&
              s.slot(8).role == Role::Categorical, "space: free / stepped / categorical roles");
    check(s.slot(9).role == Role::Fixed && s.slot(10).role == Role::Fixed && s.slot(11).role == Role::Fixed,
          "space: read-only, seed and program change are fixed");
    check(s.active().size() == 7 && s.categorical().size() == 2, "space: active and categorical sets");
    check(s.values(7).size() == 4 && std::abs(s.values(7)[1] - 1.0 / 3.0) < 1e-12, "space: categorical values");

    std::vector<double> coords = s.defaults();
    const std::vector<std::size_t> free{0, 6};
    const double pen = s.decode(free, {1.2, 0.6}, coords);
    check(std::abs(pen - 0.04) < 1e-12 && coords[0] == 1.0 && std::abs(coords[6] - 0.5) < 1e-12,
          "space: clip penalty and stepped snap");
    check(std::abs(s.encode(free, coords)[1] - 0.5) < 1e-12, "space: encode");
    check(std::abs(s.prior(s.defaults())) < 1e-15 && s.distance(s.defaults(), s.defaults()) == 0, "space: prior and distance");

    // A profile: dead parameter frozen, safe box clipped, log response warped (CLAP only), groups measured.
    Profile prof;
    auto add = [&](std::size_t i, ProbeClass c, double lo = 0, double hi = 1, Response r = Response::Linear) {
        ParamProfile q;
        q.id = ps[i].id;
        q.name = ps[i].name;
        q.measured = c;
        q.safe_lo = lo;
        q.safe_hi = hi;
        q.response = r;
        prof.params.push_back(q);
    };
    add(0, ProbeClass::Pitch);
    add(1, ProbeClass::Brightness, 0.0, 0.75, Response::Log);
    add(3, ProbeClass::Time);
    add(4, ProbeClass::Dead);
    add(5, ProbeClass::Brightness);  // "Reverb Mix" measured as brightness: hybrid trusts the probe
    prof.pairs.push_back({ps[0].id, ps[3].id, 3.0, false});  // pitch x decay interact: decay joins stage 2
    auto h = Space::build(ps, host::PluginFormat::Clap, &prof, so);
    check(h.slot(4).role == Role::Fixed && h.slot(4).why == "dead (probe)", "space: dead parameter fixed");
    check(h.slot(5).group == ParamGroup::Filter, "space: hybrid uses the measured group");
    check(h.slot(3).group == ParamGroup::Osc, "space: interacting pair fitted in one stage");
    check(h.slot(1).warp_log && std::abs(h.to_u(1, h.to_x(1, 0.3)) - 0.3) < 1e-12, "space: log warp round trip");
    coords = h.defaults();
    h.decode({1}, {1.0}, coords);
    check(std::abs(coords[1] - 0.75) < 1e-12, "space: safe box clip");
    so.mode = Mode::Guided;
    auto gsp = Space::build(ps, host::PluginFormat::Clap, &prof, so);
    check(gsp.slot(5).group == ParamGroup::Fx, "space: guided keeps the name group");
    auto v = Space::build(ps, host::PluginFormat::Vst3, &prof, SpaceOptions{});
    check(v.slot(1).warp_log && !v.slot(0).warp_log, "space: a measured poor taper is warped in either format");
    auto logp = ps;
    logp[1].log_scale = true;  // the backend already decodes it in log
    check(!Space::build(logp, host::PluginFormat::Clap, &prof, SpaceOptions{}).slot(1).warp_log, "space: no warp on top of a log decode");
    SpaceOptions pin;
    pin.pin_pitch = true;
    auto pp = Space::build(ps, host::PluginFormat::Clap, &prof, pin);
    check(pp.slot(0).role == Role::Fixed && pp.slot(1).role == Role::Free, "space: pitch class pinned when pitch is applied");
    auto named = synthetic_params();
    named[0].name = "Fine Tune";
    check(Space::build(named, host::PluginFormat::Clap, nullptr, pin).slot(0).role == Role::Fixed, "space: tuning name pinned without a profile");
    SpaceOptions only;
    only.only = {"cutoff", "13"};
    only.fix = {"13"};
    auto o = Space::build(ps, host::PluginFormat::Clap, nullptr, only);
    check(o.active().size() == 1 && o.active()[0] == 1, "space: --only and --fix");
    // Vectors from outside the search: fixed parameters back at default, steps on the grid, safe box kept.
    auto outside = s.defaults();
    outside[6] = 0.7;                // stepped, 5 values: 0.75
    outside[7] = 1.0 / 3.0 + 1e-8;   // categorical stored as float32
    outside[9] = 0.9;                // read-only
    outside[10] = 0.4;               // seed (pinned)
    outside[2] = std::nan("");
    const auto conf = s.conform(outside);
    check(std::abs(conf[6] - 0.75) < 1e-12 && conf[7] == 1.0 / 3.0 && conf[9] == s.defaults()[9] &&
              conf[10] == s.defaults()[10] && conf[2] == s.defaults()[2] && conf[0] == outside[0],
          "space: conform pins fixed parameters and snaps steps");
    auto boxed = h.defaults();
    boxed[1] = 0.9;
    check(std::abs(h.conform(boxed)[1] - 0.75) < 1e-12, "space: conform clips to the safe box");
    Rng box_rng(3);
    const auto sb = h.sample_box(std::vector<double>(h.active().size(), 1.0), box_rng);
    check(std::abs(sb[1] - 0.75) < 1e-12 && sb[4] == h.defaults()[4] && sb[9] == h.defaults()[9],
          "space: sample_box spans the safe box, fixed at default");
    SpaceOptions uns;
    uns.mode = Mode::Unsupervised;
    check(Space::build(ps, host::PluginFormat::Clap, &prof, uns).slot(1).group == ParamGroup::Unknown, "space: unsupervised has no groups");
}

void test_gp() {
    GaussianProcess gp;
    std::vector<std::vector<double>> x;
    std::vector<double> y;
    for (int i = 0; i <= 20; ++i) {
        x.push_back({i / 20.0});
        y.push_back(std::sin(6 * i / 20.0));
    }
    check(gp.fit(x, y), "GP fit");
    double err = 0, m = 0, sd = 0;
    for (int i = 0; i < 20; ++i) {
        gp.predict({(i + 0.5) / 20.0}, m, sd);
        err = std::max(err, std::abs(m - std::sin(6 * (i + 0.5) / 20.0)));
    }
    check(err < 0.05, "GP interpolates (err " + std::to_string(err) + ")");
    double sd_far = 0;
    gp.predict({3.0}, m, sd_far);
    gp.predict({0.5}, m, sd);
    check(sd_far > 10 * sd, "GP uncertainty grows away from the data");
    const auto keep = prescreen(gp, {{0.25}, {0.75}, {0.5}}, 1);  // sin(6x) is lowest near x = 0.75
    check(keep.size() == 1 && keep[0] == 1, "GP pre-screening keeps the predicted best");
}

void test_profile_and_fingerprint() {
    const auto dir = std::filesystem::temp_directory_path() / "mimicry_opt_test";
    std::filesystem::create_directories(dir);
    Profile p;
    p.format = host::PluginFormat::Vst3;
    p.uid = "ABC/def";
    p.version_hash = "1234";
    p.floor_db = 0.25;
    ParamProfile q;
    q.id = 7;
    q.name = "Cutoff";
    q.names = {ParamGroup::Filter, 0.8};
    q.measured = ProbeClass::Brightness;
    q.response = Response::Log;
    q.safe_lo = 0.25;
    q.outcomes = {Outcome::Crash, Outcome::Ok, Outcome::Ok, Outcome::Ok, Outcome::Ok};
    p.params.push_back(q);
    p.pairs.push_back({7, 9, 2.5, false});
    host::PluginInfo info;
    info.format = p.format;
    info.uid = p.uid;
    info.version_hash = p.version_hash;
    const auto path = Profile::default_path(dir, info);
    check(path.filename().string().find('/') == std::string::npos && path.extension() == ".json", "profile file name");
    check(p.save(path).has_value(), "profile save");
    auto l = Profile::load(path);
    check(l && l->matches(info) && l->params.size() == 1 && l->params[0].measured == ProbeClass::Brightness &&
              l->params[0].response == Response::Log && l->params[0].safe_lo == 0.25 &&
              l->params[0].outcomes[0] == Outcome::Crash && l->params[0].names.group == ParamGroup::Filter &&
              !l->separable(9, 7) && l->separable(7, 8) && l->floor_db == 0.25,
          "profile round trip");
    host::ParamDesc renamed = param(7, "Cutoff 2");
    check(l && !l->find(renamed) && l->find(param(7, "Cutoff")), "profile entries match on id and name");

    FingerprintDb db;
    db.format = info.format;
    db.uid = info.uid;
    db.version_hash = info.version_hash;
    db.ids = {1, 2};
    db.names = {"A", "B"};
    for (int i = 0; i < 10; ++i) {
        db.coords.push_back({float(i) / 10, 0.5f});
        db.embeddings.push_back(Embedding(kEmbeddingDims, float(i)));
    }
    const auto fp = FingerprintDb::default_path(dir, info);
    check(db.save(fp).has_value(), "fingerprint save");
    auto d2 = FingerprintDb::load(fp);
    check(d2 && d2->size() == 10 && d2->coords[3][0] == 0.3f && d2->embeddings[4][5] == 4.0f, "fingerprint round trip");
    const auto nn = d2 ? d2->nearest(Embedding(kEmbeddingDims, 6.2f), 3) : std::vector<std::size_t>{};
    check(nn.size() == 3 && nn[0] == 6 && nn[1] == 7 && nn[2] == 5, "fingerprint nearest neighbours");
    std::vector<host::ParamDesc> pd{param(1, "A"), param(2, "B")};
    check(d2 && d2->matches(info, pd), "fingerprint matches its plugin");
    pd[1].name = "C";
    check(d2 && !d2->matches(info, pd), "fingerprint rejects a different parameter table");
    std::filesystem::remove_all(dir);
}

void test_probe_safe_box() {
    // Two parameters at default 0.45: A crashes at 0.5 (the probe value next to the default), B at 0.
    std::vector<host::ParamDesc> ps{param(1, "A", 0, 0.45), param(2, "B", 0, 0.45)};
    const int rate = 48000;
    RenderBatch render = [&](const std::vector<host::Job>& jobs) {
        std::vector<expected<host::Audio, Outcome>> out;
        for (const auto& j : jobs) {
            if (j.coords[0] == 0.5 || j.coords[1] == 0.0) {
                out.push_back(unexpected(Outcome::Crash));
                continue;
            }
            host::Audio a{rate, 1, std::vector<float>(std::size_t(rate / 2))};
            for (std::size_t i = 0; i < a.samples.size(); ++i)
                a.samples[i] = float(0.3 * std::sin(2 * std::numbers::pi * 220.0 * double(i) / rate));
            out.push_back(std::move(a));
        }
        return out;
    };
    ProbeOptions po;
    po.job.sample_rate = rate;
    po.pairs = false;
    host::PluginInfo info;
    const auto prof = probe(render, info, ps, po);
    check(prof && prof->params.size() == 2, "probe runs");
    if (!prof || prof->params.size() != 2) return;
    const auto& a = prof->params[0];
    const auto& b = prof->params[1];
    check(a.safe_lo == 0.0 && a.safe_hi == 0.45, "probe: a failure next to the default narrows that side (" +
                                                       std::to_string(a.safe_lo) + ", " + std::to_string(a.safe_hi) + ")");
    check(b.safe_lo == 0.25 && b.safe_hi == 1.0, "probe: safe box cut at the last good value");
}

void test_embedding() {
    const int rate = 48000;
    auto tone = [&](double amp, double f) {
        std::vector<float> x(rate, 0.0f);
        for (int i = 2400; i < rate; ++i)
            x[std::size_t(i)] = float(amp * std::sin(2 * std::numbers::pi * f * i / rate) * std::exp(-(i - 2400) / 12000.0));
        return x;
    };
    Embedder e(rate);
    const auto a = e.embed(tone(0.5, 220)), b = e.embed(tone(0.05, 220)), c = e.embed(tone(0.5, 1760));
    Rng rng(4);
    std::vector<float> noise(rate);
    for (auto& v : noise) v = float(0.2 * rng.normal());
    const auto n = e.embed(noise);
    check(a.size() == kEmbeddingDims && embedding_distance(a, b) < 0.2, "embedding is level independent");
    check(std::abs((a[kEmbeddingLevel] - b[kEmbeddingLevel]) - 20.0f) < 0.5f, "embedding level dimension");
    check(embedding_distance(a, c) > 2.0 && embedding_distance(a, n) > 2.0, "embedding separates different sounds");
    check(c[kEmbeddingCentroid] > a[kEmbeddingCentroid] + 6 * 2, "embedding centroid follows pitch");
    const auto silent = e.embed(std::vector<float>(rate, 0.0f));
    check(silent.size() == kEmbeddingDims && std::isfinite(embedding_distance(a, silent)), "embedding of silence");
}

// Spec 05 section 4: ddmin finds the minimal failing set, bisection the thresholds; a different failure
// (NonFinite) in other parameters does not count as the same failure.
void test_constraints() {
    auto fn = [](const std::vector<double>& x) {
        if (x[2] > 0.8 && x[5] > 0.9) return Outcome::Crash;
        if (x[7] > 0.95) return Outcome::NonFinite;
        return Outcome::Ok;
    };
    long calls = 0;
    OutcomeBatch test = [&](const std::vector<std::vector<double>>& vs) {
        std::vector<Outcome> out;
        for (const auto& v : vs) out.push_back(fn(v));
        calls += long(vs.size());
        return out;
    };
    const std::vector<double> good(8, 0.3);
    std::vector<double> bad(8, 0.97);
    bad[0] = 0.3;  // unchanged: not part of the search
    const auto r = bisect_failure(test, good, bad, Outcome::Crash);
    check(r.reproduced && r.minimal == std::vector<std::size_t>{2, 5}, "bisect: minimal set {2, 5}");
    check(r.threshold.size() == 2 && std::abs(r.threshold[0] - 0.8) < 0.005 && std::abs(r.threshold[1] - 0.9) < 0.005,
          "bisect: thresholds within 0.5 % (" + (r.threshold.size() == 2 ? std::to_string(r.threshold[0]) + ", " + std::to_string(r.threshold[1]) : std::string()) + ")");
    check(r.renders == calls && r.renders < 80, "bisect: a handful of renders (" + std::to_string(r.renders) + ")");
    check(!bisect_failure(test, bad, good, Outcome::Crash).reproduced, "bisect: a failing good vector is no reference");
    check(!bisect_failure(test, good, bad, Outcome::Timeout).reproduced, "bisect: another outcome does not reproduce");

    std::vector<host::ParamDesc> ps;
    for (std::uint32_t i = 0; i < 8; ++i) ps.push_back(param(100 + i, "P" + std::to_string(i)));
    auto c = make_constraint(r, bad, Outcome::Crash, ps);
    check(c.bounds.size() == 2 && c.bounds[0].id == 102 && c.bounds[1].name == "P5" && c.bounds[0].hi == 1.0,
          "constraint: ids, names, box to the edge on the failing side");
    auto inside = good;
    inside[2] = 0.85;
    inside[5] = 0.95;
    auto outside = inside;
    outside[5] = 0.85;
    check(c.contains(inside) && !c.contains(outside) && c.contains(bad), "constraint: geometric test");
    check(c.describe().find("P2 >= 0.80") != std::string::npos, "constraint: description " + c.describe());

    ConstraintSet set(ps);
    set.add(c);
    set.add(c);  // the same region again: merged, counted
    check(set.constraints().size() == 1 && set.constraints()[0].count == 2, "constraint set: merge of the same region");
    check(set.find(inside) && !set.find(outside), "constraint set: find");
    check(set.observe_failure(inside, Outcome::Crash) && set.constraints()[0].count == 3, "constraint set: a failure inside counts");
    check(!set.observe_failure(outside, Outcome::Crash), "constraint set: a failure outside is new");
    // A valid render inside shrinks the box past it; the witness (the bisected failure) stays inside.
    auto valid = good;
    valid[2] = 0.99;
    valid[5] = 0.91;
    check(set.observe_valid(valid) == 1 && !set.find(valid) && set.find(bad), "constraint set: shrink past a valid point");
    check(set.constraints()[0].contradicted == 1 && set.constraints()[0].confidence() < 0.75, "constraint set: confidence falls");
    // The failing point itself valid: the constraint is dropped.
    auto witness = good;
    witness[2] = bad[2];
    witness[5] = bad[5];
    set.observe_valid(witness);
    check(set.empty() && set.dropped() == 1, "constraint set: dropped when its witness renders valid");

    // JSON round trip; a renamed parameter makes a stored constraint inapplicable (rule 7).
    auto js = ConstraintSet::to_json({c});
    auto back = ConstraintSet::from_json(json::parse(js.dump()).value());
    check(back && back->size() == 1 && (*back)[0].bounds.size() == 2 && std::abs((*back)[0].bounds[1].lo - c.bounds[1].lo) < 1e-12,
          "constraint json round trip");
    ConstraintSet fresh(ps);
    check(back && fresh.adopt(*back) == 1 && fresh.find(inside), "constraint set: adopt by id + name");
    auto renamed = ps;
    renamed[5].name = "P5 renamed";
    ConstraintSet other(renamed);
    check(back && other.adopt(*back) == 0, "constraint set: a renamed parameter is not matched");
    std::reverse(ps.begin(), ps.end());  // a different table order resolves to new indices
    ConstraintSet moved(ps);
    auto rev = inside;
    std::reverse(rev.begin(), rev.end());
    check(back && moved.adopt(*back) == 1 && moved.find(rev) && !moved.find(inside), "constraint set: indices resolved per table");

    Profile prof;
    prof.uid = "x";
    prof.constraints = {c};
    auto pb = Profile::from_json(prof.to_json());
    check(pb && pb->constraints.size() == 1 && !pb->probed() && pb->constraints[0].outcome == Outcome::Crash,
          "profile: constraints round trip");
}

// Synthetic objective: a quadratic bowl around `target` (dB-like), a categorical that must match, and a
// planted crash region (first two parameters both high), as the test synth's A/B hazard.
class Synthetic final : public Objective {
public:
    Synthetic(std::vector<double> target, std::size_t cat, double cat_value)
        : target_(std::move(target)), cat_(cat), cat_value_(cat_value) {}
    std::vector<Evaluation> evaluate(const std::vector<std::vector<double>>& coords, bool embed) override {
        std::vector<Evaluation> out(coords.size());
        if (delay_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
        for (std::size_t i = 0; i < coords.size(); ++i) {
            const auto& x = coords[i];
            ++calls;
            if (x[0] > 0.9 && x[1] > 0.9) {
                out[i].outcome = Outcome::Crash;
                continue;
            }
            double s = 0;
            for (std::size_t k = 0; k < target_.size(); ++k)
                if (k != cat_) s += (x[k] - target_[k]) * (x[k] - target_[k]);
            out[i].loss = 20.0 * std::sqrt(s / double(target_.size())) + (std::abs(x[cat_] - cat_value_) < 1e-9 ? 0.0 : 3.0);
            if (embed) out[i].embedding = Embedding(kEmbeddingDims, float(x[0] * 10));
        }
        return out;
    }
    long calls = 0;
    int delay_ms = 0;  // per batch, a slow plugin

private:
    std::vector<double> target_;
    std::size_t cat_;
    double cat_value_;
};

void test_pipeline() {
    // Parameters 0..5 continuous (osc, filter, env, env, lfo, fx by name), 6 stepped, 7 categorical.
    auto ps = synthetic_params();
    ps.resize(9);
    ps.erase(ps.begin() + 8);  // drop Sync: one categorical (Wave)
    std::vector<double> target{0.95, 0.4, 0.2, 0.6, 0.3, 0.1, 0.75, 2.0 / 3.0};
    Synthetic obj(target, 7, 2.0 / 3.0);
    auto space = Space::build(ps, host::PluginFormat::Clap, nullptr, SpaceOptions{});
    check(space.active().size() == 7 && space.categorical().size() == 1, "pipeline space");
    PipelineOptions po;
    po.max_evals = 6000;
    po.time_cap_s = 60;
    po.seed_samples = 200;
    po.prior = false;
    po.seed = 11;
    std::vector<std::string> stages;
    po.log = [&](const std::string& s) { stages.push_back(s); };
    const auto r = run_pipeline(obj, space, po);
    check(r.best.eval.ok() && r.best.eval.loss < 0.2, "pipeline reaches the optimum (loss " + std::to_string(r.best.eval.loss) + ")");
    check(std::abs(r.best.coords[7] - 2.0 / 3.0) < 1e-9, "pipeline: S6 finds the categorical value");
    check(std::abs(r.best.coords[6] - 0.75) < 1e-9, "pipeline: stepped parameter on its grid");
    check(r.evals <= po.max_evals + 64 && obj.calls >= r.evals, "pipeline: evaluation budget");
    long counted = 0;
    for (const auto& [o, n] : r.outcomes) counted += n;
    check(counted == r.evals, "pipeline: every evaluation's outcome is recorded");
    check(r.outcomes.count(Outcome::Crash) && r.outcomes.at(Outcome::Crash) > 0, "pipeline: crashes in the planted region are recorded");
    bool s2 = false, s5 = false, s7 = false;
    for (const auto& st : r.stages) {
        s2 = s2 || st.name.rfind("S2", 0) == 0;
        s5 = s5 || st.name.rfind("S5", 0) == 0;
        s7 = s7 || st.name.rfind("S7", 0) == 0;
    }
    check(s2 && s5 && s7 && r.stages.front().name == "S1 seed", "pipeline: stages S1, S2, S5, S7 ran");
    check(!r.top_k.empty() && r.top_k[0].fitness == r.best.fitness, "pipeline: top-K starts with the best");
    for (std::size_t i = 0; i < r.top_k.size(); ++i)
        for (std::size_t j = i + 1; j < r.top_k.size(); ++j)
            check(space.distance(r.top_k[i].coords, r.top_k[j].coords) >= po.diversity_param, "pipeline: top-K are diverse");

    // The same run learning constraints (spec 05 sections 3-4): the planted region is bisected, later samples
    // inside it are redrawn, and fewer candidates crash.
    // The optimum sits next to the corner, so a search without constraints keeps sampling into it.
    auto corner = target;
    corner[1] = 0.87;
    Synthetic nobj(corner, 7, 2.0 / 3.0), cobj(corner, 7, 2.0 / 3.0);
    po.log = nullptr;
    po.max_evals = 3000;
    const auto nr = run_pipeline(nobj, space, po);
    ConstraintSet learned(ps);
    po.constraints = &learned;
    const auto cr = run_pipeline(cobj, space, po);
    po.constraints = nullptr;
    const long crashes = nr.outcomes.count(Outcome::Crash) ? nr.outcomes.at(Outcome::Crash) : 0;
    const long ccrashes = cr.outcomes.count(Outcome::Crash) ? cr.outcomes.at(Outcome::Crash) : 0;
    bool planted = false;
    for (const auto& k : learned.constraints()) {
        bool a = false, b = false;
        for (const auto& bd : k.bounds) {
            a = a || (bd.id == 10 && std::abs(bd.lo - 0.9) < 0.01);
            b = b || (bd.id == 11 && std::abs(bd.lo - 0.9) < 0.01);
        }
        planted = planted || (a && b && k.bounds.size() == 2);
    }
    check(!cr.learned.empty() && planted, "pipeline: the planted crash region is learned");
    std::printf("constraints: crashes %ld -> %ld, %ld redrawn, %ld skipped, %d bisections (%ld renders)\n", crashes, ccrashes,
                cr.resampled, cr.skipped, cr.bisections, cr.bisect_renders);
    check(cr.resampled + cr.skipped > 0 && ccrashes < crashes,
          std::format("pipeline: constraints avoid crashes ({} -> {}, {} redrawn, {} skipped)", crashes, ccrashes, cr.resampled, cr.skipped));
    check(cr.best.eval.loss < 0.2, "pipeline with constraints still converges (loss " + std::to_string(cr.best.eval.loss) + ")");
    // Failure examples for the reports (spec 05 section 7): one per outcome and minimal set; the bisected
    // crash names its parameters.
    bool explained = false;
    for (const auto& f : cr.failures) explained = explained || (f.outcome == Outcome::Crash && f.minimal == std::vector<std::size_t>{0, 1});
    check(explained, "pipeline: the crash example carries its minimal set");
    check(!nr.failures.empty() && nr.failures.size() == 1 && nr.failures[0].minimal.empty(),
          "pipeline: without bisection one crash example, no minimal set");

    // abort stops the run between batches (a parameter table that changed, spec 05 section 9).
    Synthetic aobj(target, 7, 2.0 / 3.0);
    PipelineOptions ao = po;
    ao.max_evals = 0;
    ao.abort = [&aobj] { return aobj.calls >= 300; };
    const auto ar = run_pipeline(aobj, space, ao);
    check(ar.aborted && ar.evals < 400 && ar.top_k.empty(), "pipeline: abort stops the run (" + std::to_string(ar.evals) + " evals)");

    // Unsupervised: no group stages.
    SpaceOptions uo;
    uo.mode = Mode::Unsupervised;
    auto us = Space::build(ps, host::PluginFormat::Clap, nullptr, uo);
    Synthetic obj2(target, 7, 2.0 / 3.0);
    po.log = nullptr;
    po.max_evals = 3000;
    const auto u = run_pipeline(obj2, us, po);
    bool groups = false;
    for (const auto& st : u.stages) groups = groups || st.name.rfind("S2", 0) == 0 || st.name.rfind("S3", 0) == 0;
    check(!groups && u.best.eval.loss < 1.0, "pipeline: unsupervised skips S2-S4 and converges (loss " + std::to_string(u.best.eval.loss) + ")");

    // Time cap.
    Synthetic obj3(target, 7, 2.0 / 3.0);
    obj3.delay_ms = 5;
    po.max_evals = 0;
    po.time_cap_s = 0.3;
    const auto t = run_pipeline(obj3, space, po);
    check(t.time_capped && t.seconds < 2.0, "pipeline: time cap");

    // sep-CMA-ES above the threshold, and the surrogate forced on.
    std::vector<host::ParamDesc> many;
    for (std::uint32_t i = 0; i < 80; ++i) many.push_back(param(i, "P" + std::to_string(i)));
    std::vector<double> tgt(80);
    Rng rng(5);
    for (auto& v : tgt) v = rng.uniform(0.2, 0.8);
    tgt[0] = tgt[1] = 0.5;
    Synthetic big(tgt, 79, tgt[79]);  // the "categorical" index is a free one here: no penalty term
    auto bs = Space::build(many, host::PluginFormat::Clap, nullptr, SpaceOptions{});
    PipelineOptions bo;
    bo.max_evals = 4000;
    bo.time_cap_s = 60;
    bo.seed_samples = 100;
    bo.restarts = 1;
    std::string polish;
    bo.log = [&](const std::string& s) {
        if (s.rfind("S5", 0) == 0) polish = s;
    };
    const auto br = run_pipeline(big, bs, bo);
    check(polish.find("sep-CMA-ES") != std::string::npos, "pipeline: sep-CMA-ES above 64 parameters");
    check(br.best.eval.loss < br.stages.front().after, "pipeline: sep-CMA-ES improves on the seeds");

    // Seeds from outside (a fingerprint built without --fix) cannot move a fixed parameter off its default.
    SpaceOptions fo;
    fo.fix = {"12"};  // Attack
    auto fs = Space::build(ps, host::PluginFormat::Clap, nullptr, fo);
    auto ftarget = target;
    ftarget[2] = 0.9;  // the optimum needs Attack off its default, so an unconformed seed would win
    const auto seed = ftarget;
    Synthetic fobj(ftarget, 7, 2.0 / 3.0);
    PipelineOptions fpo;
    fpo.max_evals = 600;
    fpo.seed_samples = 40;
    fpo.prior = false;
    const auto fr = run_pipeline(fobj, fs, fpo, {seed});
    bool pinned = fr.best.coords[2] == fs.defaults()[2];
    for (const auto& c : fr.top_k) pinned = pinned && c.coords[2] == fs.defaults()[2];
    check(pinned, "pipeline: a seed's fixed parameter is reset to its default");

    Synthetic small(target, 7, 2.0 / 3.0);
    PipelineOptions so;
    so.max_evals = 800;
    so.surrogate_above_s = 0;
    so.seed_samples = 60;
    const auto sr = run_pipeline(small, space, so);
    check(sr.surrogate_used && sr.best.eval.loss < 3.0, "pipeline: surrogate pre-screening runs (loss " + std::to_string(sr.best.eval.loss) + ")");
}

}  // namespace

int main() {
    test_jacobi();
    test_cma();
    test_sampling();
    test_semantics();
    test_space();
    test_gp();
    test_profile_and_fingerprint();
    test_probe_safe_box();
    test_embedding();
    test_constraints();
    test_pipeline();
    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::puts("mimicry_opt_test: all passed");
    return 0;
}
