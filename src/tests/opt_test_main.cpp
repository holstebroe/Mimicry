// Tests for WP3 that need no plugin: CMA-ES and sep-CMA-ES on known functions (T5, docs/spec/
// 07_OUTPUT_VALIDATION.md), the Jacobi eigensolver, sampling, semantics, the encoding, the surrogate, the
// profile and fingerprint files, the embedding, and the pipeline on a synthetic objective with a planted
// crash region.

#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <numbers>
#include <string>
#include <thread>
#include <vector>

#include "core/Cma.hpp"
#include "core/ParamSemantics.hpp"
#include "core/Rng.hpp"
#include "core/Sampling.hpp"
#include "opt/Embedding.hpp"
#include "opt/Fingerprint.hpp"
#include "opt/Pipeline.hpp"
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
    test_embedding();
    test_pipeline();
    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::puts("mimicry_opt_test: all passed");
    return 0;
}
