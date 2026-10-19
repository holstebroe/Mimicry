// mimicry: supervisor / command line (docs/spec/01_ARCHITECTURE.md section 6).
// Implemented: list, probe, fit, fingerprint. The rest are implemented in the order of docs/PROJECT_PLAN.md.
// Plugins are only ever loaded in mimicry_worker processes (CLAUDE.md rule 4); --no-isolation renders
// in-process for debugging.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "core/Json.hpp"
#include "core/Version.hpp"
#include "core/Wav.hpp"
#include "features/Loss.hpp"
#include "features/Pitch.hpp"
#include "host/Codec.hpp"
#include "host/PluginHost.hpp"
#include "opt/Fingerprint.hpp"
#include "opt/Objective.hpp"
#include "opt/Pipeline.hpp"
#include "opt/Probe.hpp"
#include "opt/Profile.hpp"
#include "opt/Space.hpp"
#include "supervisor/WorkerPool.hpp"

namespace {

using namespace mimicry;
namespace fs = std::filesystem;

struct Command {
    std::string_view name;
    std::string_view usage;
    std::string_view summary;
};

constexpr Command kCommands[] = {
    {"list", "list <plugin>", "enumerate parameters with units, flags and semantics"},
    {"probe", "probe <plugin>", "sensitivity, safe range and hazard probe; writes the plugin profile"},
    {"fit", "fit --plugin P --wav T.wav", "match a WAV"},
    {"transfer", "transfer --source A --target B", "match source presets in another plugin (any format mix)"},
    {"image", "image --plugin P --image I.png", "match an image"},
    {"fingerprint", "fingerprint <plugin>", "build the random-preset feature database"},
    {"rehearse", "rehearse <plugin>", "round-trip self-test"},
    {"eval", "eval --plugin P --state S --wav T.wav", "score a state without fitting"},
    {"replay", "replay <crash.json>", "re-run a recorded failure in-process"},
};

void print_usage() {
    std::string text = std::format("mimicry {}: preset matcher for CLAP and VST3 plugins\n\nusage: mimicry <command> ...\n\n",
                                   mimicry::version);
    for (const auto& c : kCommands) text += std::format("  {:<40} {}\n", c.usage, c.summary);
    text += R"(
common options:
  --workers N          worker processes (default: cores - 1)
  --no-isolation       render in this process (debugging only)
  --profiles DIR       plugin profiles and fingerprint databases (default: profiles)
  --note C3            target pitch override; --gate MS note length

fit options:
  --minutes M          time cap (default 20)        --patience S   seconds without improvement (240)
  --evals N            evaluation budget            --mode hybrid|guided|unsupervised
  --only a,b / --fix a,b   parameter names or ids   --stages 1,5,7
  --restarts R (3)     --top-k K (3)                --seed N        --no-prior
  --kind note|hit|texture|loop                      --w-<feature> N
  --solve-gain         closed-form gain even with a level parameter
  --out DIR            run directory root (default: runs)

probe options: --no-pairs      fingerprint options: --count N (2000)

<plugin> is a .clap or .vst3 path. transfer, image, rehearse, eval and replay are not implemented yet;
see docs/PROJECT_PLAN.md for the order.
)";
    std::fputs(text.c_str(), stdout);
}

int not_implemented(std::string_view command) {
    std::fputs(std::format("mimicry {}: not implemented yet (see docs/PROJECT_PLAN.md)\n", command).c_str(), stderr);
    return 2;
}

int fail(std::string_view command, const std::string& message) {
    std::fputs(std::format("mimicry {}: {}\n", command, message).c_str(), stderr);
    return 1;
}

// --key value and --flag options plus positional arguments.
struct Args {
    std::map<std::string, std::string, std::less<>> opts;
    std::vector<std::string> positional;
    [[nodiscard]] bool has(std::string_view k) const { return opts.count(std::string(k)) != 0; }
    [[nodiscard]] std::string get(std::string_view k, std::string fallback = {}) const {
        auto it = opts.find(k);
        return it == opts.end() ? fallback : it->second;
    }
    [[nodiscard]] double num(std::string_view k, double fallback) const {
        auto it = opts.find(k);
        if (it == opts.end()) return fallback;
        try {
            return std::stod(it->second);
        } catch (...) {
            return fallback;
        }
    }
};

constexpr std::string_view kFlags[] = {"--no-isolation", "--no-prior", "--no-pairs", "--solve-gain"};

Args parse_args(int argc, char** argv, int first) {
    Args a;
    for (int i = first; i < argc; ++i) {
        std::string s = argv[i];
        if (s.rfind("--", 0) == 0) {
            const bool flag = std::find(std::begin(kFlags), std::end(kFlags), s) != std::end(kFlags);
            if (!flag && i + 1 < argc) a.opts[s] = argv[++i];
            else a.opts[s] = "1";
        } else {
            a.positional.push_back(std::move(s));
        }
    }
    return a;
}

std::vector<std::string> split(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == ',') {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

// A plugin behind a RenderBatch: a worker pool, or (--no-isolation) one in-process instance.
struct Plugin {
    std::unique_ptr<supervisor::WorkerPool> pool;
    std::unique_ptr<host::Instance> local;
    std::mutex local_mutex;
    host::PluginInfo info;
    std::vector<host::ParamDesc> params;

    opt::RenderBatch batch() {
        if (pool) return [this](const std::vector<host::Job>& jobs) { return pool->render_all(jobs); };
        return [this](const std::vector<host::Job>& jobs) {
            std::lock_guard lock(local_mutex);
            std::vector<expected<host::Audio, Outcome>> out;
            for (const auto& j : jobs) out.push_back(local->render(j));
            return out;
        };
    }
    int workers() const { return pool ? pool->size() : 1; }
};

expected<std::unique_ptr<Plugin>, std::string> open_plugin(const std::string& path, const Args& args) {
    if (auto f = host::format_from_path(path); !f) return unexpected(f.error());
    auto p = std::make_unique<Plugin>();
    if (args.has("--no-isolation")) {
        auto inst = host::open(path);
        if (!inst) return unexpected(inst.error().message);
        p->local = std::move(*inst);
        p->info = p->local->info();
        p->params = p->local->parameters();
        return p;
    }
    const int cores = int(std::max(1u, std::thread::hardware_concurrency()));
    const int workers = int(args.num("--workers", std::max(1, cores - 1)));
    auto pool = supervisor::WorkerPool::open(path, {}, workers);
    if (!pool) return unexpected(pool.error().message);
    p->pool = std::move(*pool);
    p->info = p->pool->client(0).info();
    p->params = p->pool->client(0).parameters();
    return p;
}

fs::path profiles_dir(const Args& a) { return a.get("--profiles", "profiles"); }

std::optional<opt::Profile> load_profile(const Args& a, const host::PluginInfo& info) {
    const auto path = opt::Profile::default_path(profiles_dir(a), info);
    if (!fs::exists(path)) return std::nullopt;
    auto p = opt::Profile::load(path);
    if (!p || !p->matches(info)) {
        std::fputs(std::format("note: ignoring profile {} ({})\n", path.string(), p ? "different plugin build" : p.error()).c_str(), stderr);
        return std::nullopt;
    }
    return std::move(*p);
}

host::Job note_job(const Args& a, int key, double cents, double gate_ms) {
    host::Job job;
    job.notes.push_back({key, cents, 100.0f / 127.0f, 0, a.num("--gate", gate_ms)});
    return job;
}

std::string flags_text(const host::ParamFlags& f) {
    std::string s;
    if (f.hidden) s += "hidden ";
    if (f.readonly) s += "readonly ";
    if (f.bypass) s += "bypass ";
    if (f.program_change) s += "program ";
    if (!f.automatable) s += "no-automation ";
    return s;
}

int cmd_list(const Args& a) {
    if (a.positional.empty()) return fail("list", "usage: mimicry list <plugin>");
    auto plugin = open_plugin(a.positional[0], a);
    if (!plugin) return fail("list", plugin.error());
    const auto& p = **plugin;
    const auto profile = load_profile(a, p.info);
    std::string out = std::format("{} {} ({}), uid {}, version {}\n", host::to_string(p.info.format), p.info.name,
                                  p.info.vendor, p.info.uid, p.info.version_hash);
    const auto space = opt::Space::build(p.params, p.info.format, profile ? &*profile : nullptr, opt::SpaceOptions{});
    out += std::format("{:>8}  {:<24} {:<14} {:<6} {:>5}  {:<12} {:<12} {}\n", "id", "name", "module", "units", "steps",
                       "group", "role", "flags");
    for (std::size_t i = 0; i < p.params.size(); ++i) {
        const auto& d = p.params[i];
        const auto& s = space.slot(i);
        out += std::format("{:>8}  {:<24} {:<14} {:<6} {:>5}  {:<6} {:.2f}  {:<12} {}{}\n", d.id, d.name, d.module, d.units,
                           d.steps, to_string(s.group), s.names.confidence, to_string(s.role), flags_text(d.flags),
                           s.why.empty() ? "" : "(" + s.why + ")");
    }
    std::fputs(out.c_str(), stdout);
    return 0;
}

int cmd_probe(const Args& a) {
    if (a.positional.empty()) return fail("probe", "usage: mimicry probe <plugin>");
    auto plugin = open_plugin(a.positional[0], a);
    if (!plugin) return fail("probe", plugin.error());
    auto& p = **plugin;
    int key = 48;
    if (a.has("--note")) {
        auto k = features::parse_note_name(a.get("--note"));
        if (!k) return fail("probe", "bad --note");
        key = *k;
    }
    opt::ProbeOptions po;
    po.job = note_job(a, key, 0, 800);
    po.job.tail_max_ms = 3000;
    po.pairs = !a.has("--no-pairs");
    po.log = [](const std::string& s) { std::fputs((s + "\n").c_str(), stdout); };
    std::fputs(std::format("probing {} parameters with {} worker(s)...\n", p.params.size(), p.workers()).c_str(), stdout);
    auto prof = opt::probe(p.batch(), p.info, p.params, po);
    if (!prof) return fail("probe", prof.error());
    std::string out;
    for (const auto& e : prof->params) {
        std::string oc;
        for (auto o : e.outcomes)
            if (o != Outcome::Ok) oc += std::string(to_string(o)) + " ";
        out += std::format("  {:<24} {:<7} {:<10} effect {:7.2f}  safe [{:.2f}, {:.2f}] {}\n", e.name,
                           to_string(e.names.group), to_string(e.measured), e.effect, e.safe_lo, e.safe_hi, oc);
    }
    long joined = 0;
    for (const auto& pr : prof->pairs) joined += pr.separable ? 0 : 1;
    out += std::format("floor {:.3f} dB, {:.1f} ms per render, {} pairs probed, {} interact\n", prof->floor_db,
                       prof->render_ms, prof->pairs.size(), joined);
    const auto path = opt::Profile::default_path(profiles_dir(a), p.info);
    if (const auto old = load_profile(a, p.info)) prof->constraints = old->constraints;  // learned by earlier fits
    if (auto r = prof->save(path); !r) return fail("probe", r.error());
    out += "wrote " + path.string() + "\n";
    std::fputs(out.c_str(), stdout);
    return 0;
}

int cmd_fingerprint(const Args& a) {
    if (a.positional.empty()) return fail("fingerprint", "usage: mimicry fingerprint <plugin>");
    auto plugin = open_plugin(a.positional[0], a);
    if (!plugin) return fail("fingerprint", plugin.error());
    auto& p = **plugin;
    const auto profile = load_profile(a, p.info);
    if (!profile || !profile->probed())
        std::fputs("note: no profile; run `mimicry probe` first for a safe box and dead parameters\n", stderr);
    const auto space = opt::Space::build(p.params, p.info.format, profile ? &*profile : nullptr, opt::SpaceOptions{});
    opt::FingerprintOptions fo;
    int key = 48;
    if (a.has("--note")) key = features::parse_note_name(a.get("--note")).value_or(48);
    fo.job = note_job(a, key, 0, 800);
    fo.job.tail_max_ms = 3000;
    fo.count = long(a.num("--count", 2000));
    fo.seed = std::uint64_t(a.num("--seed", 1));
    fo.threads = p.workers();
    auto last = std::chrono::steady_clock::now();
    fo.progress = [&](long done, long total) {
        if (std::chrono::steady_clock::now() - last < std::chrono::seconds(15) && done < total) return;
        last = std::chrono::steady_clock::now();
        std::fputs(std::format("  {} / {}\n", done, total).c_str(), stdout);
    };
    const auto db = opt::build_fingerprints(p.batch(), p.info, space, fo);
    const auto path = opt::FingerprintDb::default_path(profiles_dir(a), p.info);
    if (auto r = db.save(path); !r) return fail("fingerprint", r.error());
    std::fputs(std::format("{} valid of {} rendered; wrote {}\n", db.size(), db.rendered, path.string()).c_str(), stdout);
    return 0;
}

json::Value params_json(const host::PluginInfo& info, const std::vector<host::ParamDesc>& params,
                        const std::vector<double>& coords, const std::function<std::string(std::uint32_t, double)>& text) {
    json::Value v;
    v.set("format", std::string(host::to_string(info.format)));
    v.set("plugin", info.name);
    v.set("uid", info.uid);
    v.set("version_hash", info.version_hash);
    json::Array ps;
    for (std::size_t i = 0; i < params.size(); ++i) {
        json::Value e;
        e.set("id", params[i].id);
        e.set("name", params[i].name);
        e.set("module", params[i].module);
        e.set("normalised", coords[i]);
        e.set("display", text(params[i].id, coords[i]));
        ps.push_back(std::move(e));
    }
    v.set("params", json::Value(std::move(ps)));
    return v;
}

void write_text(const fs::path& path, const std::string& text) {
    std::ofstream f(path, std::ios::binary);
    f << text;
}

int cmd_fit(const Args& a) {
    const std::string plugin_path = a.get("--plugin"), wav_path = a.get("--wav");
    if (plugin_path.empty() || wav_path.empty()) return fail("fit", "usage: mimicry fit --plugin P --wav T.wav");
    auto wav = read_wav(wav_path);
    if (!wav) return fail("fit", wav.error());
    features::AnalyzeOptions ao;
    if (a.has("--note")) {
        ao.key = features::parse_note_name(a.get("--note"));
        if (!ao.key) return fail("fit", "bad --note");
    }
    if (a.has("--kind")) {
        ao.kind = features::parse_kind(a.get("--kind"));
        if (!ao.kind) return fail("fit", "bad --kind");
    }
    auto target = features::Target::from_wav(*wav, ao);
    if (!target) return fail("fit", target.error());
    const auto& an = target->analysis();
    std::fputs(std::format("target: {}, f0 {:.2f} Hz (confidence {:.2f}), key {} {:+.1f} cents, {:.0f} ms\n",
                           features::to_string(an.kind), an.f0, an.confidence, an.key, an.cents, an.duration_ms).c_str(), stdout);

    auto plugin = open_plugin(plugin_path, a);
    if (!plugin) return fail("fit", plugin.error());
    auto& p = **plugin;
    const auto stored = load_profile(a, p.info);
    // A profile written by an earlier fit before any probe carries only constraints.
    const auto profile = stored && stored->probed() ? stored : std::nullopt;
    if (!profile) std::fputs("note: no plugin profile; `mimicry probe` first makes the fit faster and safer\n", stderr);
    opt::ConstraintSet constraints(p.params);
    if (stored) {
        const auto n = constraints.adopt(stored->constraints);
        if (n) std::fputs(std::format("profile: {} hazard constraint(s)\n", n).c_str(), stdout);
    }

    // S0: the target's pitch is applied to the note (spec 04 section 4), so pitch-only parameters are pinned.
    const bool pitched = an.f0 > 0 && an.kind == features::SampleKind::Note;
    host::Job job = note_job(a, pitched ? an.key : 60, pitched ? an.cents : 0.0, std::max(50.0, an.duration_ms));
    job.sample_rate = ao.sample_rate;
    opt::SpaceOptions so;
    const std::string mode = a.get("--mode", "hybrid");
    so.mode = mode == "guided" ? opt::Mode::Guided : mode == "unsupervised" ? opt::Mode::Unsupervised : opt::Mode::Hybrid;
    so.only = split(a.get("--only"));
    so.fix = split(a.get("--fix"));
    so.pin_pitch = pitched;
    const auto space = opt::Space::build(p.params, p.info.format, profile ? &*profile : nullptr, so);

    auto weights = features::Weights::preset(an.kind);
    for (const auto& [k, v] : a.opts)
        if (k.rfind("--w-", 0) == 0 && !weights.set(k.substr(4), a.num(k, 0))) return fail("fit", "unknown weight " + k);
    features::ScorerOptions sco;
    sco.solve_gain = a.has("--solve-gain") || space.level_index() < 0;
    opt::AudioObjective obj(p.batch(), job, *target, weights, sco, p.workers());
    if (profile) {
        const auto rp = features::repeat_policy(profile->floor_db);
        obj.set_renders_per_candidate(rp.renders);
    }
    if (!obj.calibrate_offset(space.defaults()))
        std::fputs("note: the defaults do not render; no onset offset fitted\n", stderr);

    std::vector<std::vector<double>> seeds;
    if (auto db = opt::FingerprintDb::load(opt::FingerprintDb::default_path(profiles_dir(a), p.info)); db) {
        if (db->matches(p.info, p.params)) {
            opt::Embedder em(job.sample_rate);
            for (auto i : db->nearest(em.embed(target->mono()), 8)) seeds.emplace_back(db->coords[i].begin(), db->coords[i].end());
            std::fputs(std::format("fingerprint: {} nearest neighbours seed S1\n", seeds.size()).c_str(), stdout);
        }
    }

    opt::PipelineOptions po;
    po.time_cap_s = 60.0 * a.num("--minutes", 20);
    po.patience_s = a.num("--patience", po.patience_s);
    po.max_evals = long(a.num("--evals", 0));
    po.restarts = int(a.num("--restarts", po.restarts));
    po.top_k = int(a.num("--top-k", po.top_k));
    po.seed = std::uint64_t(a.num("--seed", 1));
    po.prior = !a.has("--no-prior");
    po.constraints = &constraints;
    for (const auto& s : split(a.get("--stages"))) po.stages.push_back(std::atoi(s.c_str()));
    po.log = [](const std::string& s) { std::fputs((s + "\n").c_str(), stdout); };
    po.progress = [](const opt::Progress& pr) {
        std::fputs(std::format("  [{:6.0f} s] {:<24} {:7} evals, best {:.3f} dB\n", pr.seconds, pr.stage, pr.evals, pr.best).c_str(), stdout);
    };
    std::fputs(std::format("fit: {} {} with {} active and {} categorical parameters, {} worker(s), mode {}\n",
                           host::to_string(p.info.format), p.info.name, space.active().size(), space.categorical().size(),
                           p.workers(), to_string(so.mode)).c_str(), stdout);
    const auto res = opt::run_pipeline(obj, space, po, seeds);
    // Learned and updated constraints go back into the profile (spec 05 section 4), even when the fit failed.
    if (!constraints.empty() || (stored && !stored->constraints.empty())) {
        opt::Profile out = stored ? *stored : opt::Profile{};
        if (!stored) {
            out.format = p.info.format;
            out.uid = p.info.uid;
            out.name = p.info.name;
            out.vendor = p.info.vendor;
            out.version_hash = p.info.version_hash;
        }
        out.constraints = constraints.constraints();
        const auto path = opt::Profile::default_path(profiles_dir(a), p.info);
        if (auto r = out.save(path); !r) std::fputs(std::format("note: {}\n", r.error()).c_str(), stderr);
    }
    if (!res.best.eval.ok()) return fail("fit", "no valid candidate was found");

    // Run directory (spec 07 section 1).
    char stamp[32];
    const std::time_t now = std::time(nullptr);
    std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", std::localtime(&now));
    const fs::path dir = fs::path(a.get("--out", "runs")) /
                         std::format("{}-{}-{}", stamp, fs::path(plugin_path).stem().string(), fs::path(wav_path).stem().string());
    std::error_code ec;
    fs::create_directories(dir / "top-k", ec);
    if (ec) return fail("fit", "cannot create " + dir.string());
    auto text = [&](std::uint32_t id, double u) {
        if (p.pool) return p.pool->value_text(id, u);
        std::lock_guard lock(p.local_mutex);
        return p.local->value_text(id, u);
    };
    auto save_match = [&](const opt::Candidate& c, const fs::path& stem) {
        auto pj = params_json(p.info, p.params, c.coords, text);
        pj.set("loss_db", c.eval.loss);
        pj.set("gain_db", c.eval.score.gain_db);
        json::Object losses;
        for (std::size_t f = 0; f < features::kFeatureCount; ++f)
            losses[std::string(features::to_string(features::Feature(f)))] = c.eval.score.loss[f];
        pj.set("features", json::Value(std::move(losses)));
        pj.set("notes", host::to_json(job)["notes"]);
        write_text(fs::path(stem.string() + ".params.json"), pj.dump(2) + "\n");
        if (auto audio = obj.render(c.coords)) {
            WavData w{audio->sample_rate, audio->channels, audio->samples};
            (void)write_wav(fs::path(stem.string() + ".wav"), w);
        }
    };
    save_match(res.best, dir / "best");
    fs::rename(dir / "best.wav", dir / "match.wav", ec);
    for (std::size_t i = 0; i < res.top_k.size(); ++i) save_match(res.top_k[i], dir / "top-k" / std::to_string(i + 1));

    // ab.wav: target, 0.3 s of silence, match scaled by the solved gain (same scale for both) [S].
    if (auto audio = obj.render(res.best.coords)) {
        const auto mono = opt::mono_of(*audio);
        const float g = float(std::pow(10.0, res.best.eval.score.gain_db / 20.0));
        WavData ab{job.sample_rate, 1, target->mono()};
        ab.samples.resize(ab.samples.size() + std::size_t(0.3 * job.sample_rate), 0.0f);
        for (float s : mono) ab.samples.push_back(s * g);
        (void)write_wav(dir / "ab.wav", ab);
    }
    // best.state: the plugin's own state after rendering the match (a fresh worker on this thread).
    if (p.pool) {
        if (auto c = supervisor::WorkerClient::open(plugin_path); c) {
            host::Job j = job;
            j.coords = res.best.coords;
            if ((*c)->render(j)) {
                if (auto st = (*c)->save_state()) {
                    std::ofstream f(dir / "best.state", std::ios::binary);
                    f.write(reinterpret_cast<const char*>(st->data()), std::streamsize(st->size()));
                }
            }
        }
    }

    json::Value result;
    result.set("format", std::string(host::to_string(p.info.format)));
    result.set("plugin", p.info.name);
    result.set("uid", p.info.uid);
    result.set("version_hash", p.info.version_hash);
    result.set("target", wav_path);
    result.set("kind", std::string(features::to_string(an.kind)));
    result.set("mode", std::string(to_string(so.mode)));
    result.set("loss_db", res.best.eval.loss);
    result.set("gain_db", res.best.eval.score.gain_db);
    result.set("evals", static_cast<std::int64_t>(res.evals));
    result.set("seconds", res.seconds);
    result.set("time_capped", res.time_capped);
    result.set("surrogate", res.surrogate_used);
    if (profile) result.set("floor_db", profile->floor_db);
    json::Array stages;
    for (const auto& s : res.stages) {
        json::Value st;
        st.set("name", s.name);
        st.set("free", s.free);
        st.set("before_db", s.before);
        st.set("after_db", s.after);
        st.set("evals", static_cast<std::int64_t>(s.evals));
        st.set("seconds", s.seconds);
        stages.push_back(std::move(st));
    }
    result.set("stages", json::Value(std::move(stages)));
    json::Object outcomes;
    for (const auto& [o, n] : res.outcomes) outcomes[std::string(to_string(o))] = static_cast<std::int64_t>(n);
    result.set("outcomes", json::Value(std::move(outcomes)));
    json::Value hz;
    hz.set("learned", opt::ConstraintSet::to_json(res.learned));
    hz.set("known", static_cast<std::int64_t>(constraints.constraints().size()));
    hz.set("dropped", constraints.dropped());
    hz.set("bisections", res.bisections);
    hz.set("bisect_renders", static_cast<std::int64_t>(res.bisect_renders));
    hz.set("resampled", static_cast<std::int64_t>(res.resampled));
    hz.set("skipped", static_cast<std::int64_t>(res.skipped));
    result.set("constraints", std::move(hz));
    json::Array topk;
    for (const auto& t : res.top_k) topk.push_back(t.eval.loss);
    result.set("top_k_loss_db", json::Value(std::move(topk)));
    write_text(dir / "result.json", result.dump(2) + "\n");

    std::string summary = std::format("\nbest {:.3f} dB (gain {:+.1f} dB) after {} evaluations in {:.0f} s\n", res.best.eval.loss,
                                      res.best.eval.score.gain_db, res.evals, res.seconds);
    for (const auto& s : res.stages)
        summary += std::format("  {:<26} {:3} free  {:8.3f} -> {:8.3f} dB  {:6} evals\n", s.name, s.free, s.before, s.after, s.evals);
    for (const auto& [o, n] : res.outcomes)
        if (o != Outcome::Ok) summary += std::format("  {} {} candidate(s)\n", n, to_string(o));
    for (const auto& c : res.learned) summary += "  learned: " + c.describe() + "\n";
    if (res.resampled + res.skipped > 0)
        summary += std::format("  {} sample(s) redrawn and {} seed point(s) skipped inside known constraints\n",
                               res.resampled, res.skipped);
    summary += "wrote " + dir.string() + " (listen to ab.wav)\n";
    std::fputs(summary.c_str(), stdout);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        print_usage();
        return 1;
    }
    const std::string_view cmd = argv[1];
    if (cmd == "--help" || cmd == "-h" || cmd == "help") {
        print_usage();
        return 0;
    }
    if (cmd == "--version") {
        std::fputs(std::format("mimicry {}\n", mimicry::version).c_str(), stdout);
        return 0;
    }
    const Args args = parse_args(argc, argv, 2);
    if (cmd == "list") return cmd_list(args);
    if (cmd == "probe") return cmd_probe(args);
    if (cmd == "fit") return cmd_fit(args);
    if (cmd == "fingerprint") return cmd_fingerprint(args);
    for (const auto& c : kCommands)
        if (c.name == cmd) return not_implemented(cmd);
    std::fputs(std::format("mimicry: unknown command '{}' (try --help)\n", cmd).c_str(), stderr);
    return 1;
}
