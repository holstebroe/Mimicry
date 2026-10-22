#include "targets/Harvest.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <format>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <sstream>

#include "host/Codec.hpp"
#include "opt/Embedding.hpp"
#include "opt/Objective.hpp"

namespace mimicry::targets {

namespace fs = std::filesystem;
using host::PresetMethod;

std::string_view to_string(PresetOutcome o) noexcept {
    switch (o) {
        case PresetOutcome::Ok: return "ok";
        case PresetOutcome::NoChange: return "no-change";
        case PresetOutcome::Duplicate: return "duplicate";
        case PresetOutcome::NotReproducible: return "not-reproducible";
        case PresetOutcome::LoadError: return "load-error";
        case PresetOutcome::Timeout: return "timeout";
    }
    return "unknown";
}

int HarvestResult::ok_count() const {
    return static_cast<int>(std::ranges::count_if(entries, [](const HarvestEntry& e) { return e.outcome == PresetOutcome::Ok; }));
}

int ladder_rank(PresetMethod m) noexcept {
    switch (m) {
        case PresetMethod::File: return 0;
        case PresetMethod::ClapPreset: return 1;
        case PresetMethod::ProgramData: return 2;
        case PresetMethod::ProgramList: return 3;
        case PresetMethod::MidiProgram: return 4;
        case PresetMethod::ProgramParam: return 5;
        case PresetMethod::Capture: return 6;
    }
    return 7;
}

std::string state_hash(const std::vector<std::uint8_t>& state) {
    std::uint64_t h = 1469598103934665603ull;
    for (const std::uint8_t b : state) {
        h ^= b;
        h *= 1099511628211ull;
    }
    return std::format("{:016x}-{}", h, state.size());
}

namespace {

std::string lower(std::string s) {
    std::ranges::transform(s, s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// Methods that cannot say how many slots exist: a run of empty slots ends the bank. Program lists and
// program data report their program count, so every slot is tried.
bool blind(PresetMethod m) { return m == PresetMethod::MidiProgram; }

// The standard VST3 preset folders of a plugin [S: VST3 SDK, "Preset Locations"]: user, then shared.
std::vector<fs::path> vst3_preset_folders(const host::PluginInfo& info) {
    std::vector<fs::path> roots;
    auto env = [](const char* name) -> std::string {
        const char* v = std::getenv(name);
        return v ? v : "";
    };
#if defined(_WIN32)
    if (!env("USERPROFILE").empty()) roots.push_back(fs::path(env("USERPROFILE")) / "Documents" / "VST3 Presets");
    if (!env("APPDATA").empty()) roots.push_back(fs::path(env("APPDATA")) / "VST3 Presets");
    if (!env("PROGRAMDATA").empty()) roots.push_back(fs::path(env("PROGRAMDATA")) / "VST3 Presets");
#elif defined(__APPLE__)
    if (!env("HOME").empty()) roots.push_back(fs::path(env("HOME")) / "Library" / "Audio" / "Presets");
    roots.emplace_back("/Library/Audio/Presets");
#else
    if (!env("HOME").empty()) roots.push_back(fs::path(env("HOME")) / ".vst3" / "presets");
    roots.emplace_back("/usr/share/vst3/presets");
    roots.emplace_back("/usr/local/share/vst3/presets");
#endif
    std::vector<fs::path> out;
    for (const auto& r : roots) out.push_back(r / info.vendor / info.name);
    return out;
}

void add_folder(const fs::path& dir, bool any, const std::vector<std::string>& exts, std::vector<host::PresetRef>& out) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return;
    std::vector<fs::path> files;
    for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const std::string ext = lower(it->path().extension().string());
        if (any || std::ranges::find(exts, ext) != exts.end()) files.push_back(it->path());
    }
    std::ranges::sort(files);
    for (const auto& f : files) {
        host::PresetRef r;
        r.method = PresetMethod::File;
        r.location = f.string();
        r.name = f.stem().string();
        out.push_back(std::move(r));
    }
}

// Identity of a preset as the plugin offers it, for "already tried" in a resumed harvest.
std::string ref_key(const host::PresetRef& r) {
    return std::format("{}|{}|{}|{}|{}|{}|{}|{}|{}", host::to_string(r.method), r.location, r.load_key, r.list_id, r.param_id,
                       r.index, r.bank, r.name, r.count);
}

PresetOutcome parse_outcome_name(std::string_view t) {
    for (const auto o : {PresetOutcome::Ok, PresetOutcome::NoChange, PresetOutcome::Duplicate, PresetOutcome::NotReproducible,
                         PresetOutcome::LoadError, PresetOutcome::Timeout})
        if (to_string(o) == t) return o;
    return PresetOutcome::LoadError;
}

struct Rendered {
    bool ok = false;
    std::string why;
    opt::Embedding emb;
    double level = 0;
};

// Difference of two renders: the embedding distance (level free) or the level difference, whichever is
// larger, so a preset that only changes the level still counts [D].
double difference(const Rendered& a, const Rendered& b) {
    if (!a.ok || !b.ok) return 1e9;
    return std::max(opt::embedding_distance(a.emb, b.emb), std::fabs(a.level - b.level));
}

}  // namespace

std::vector<host::PresetRef> file_presets(const HarvestOptions& options, const host::PluginInfo& info) {
    std::vector<std::string> exts = {".state"};
    if (info.format == host::PluginFormat::Vst3) exts.push_back(".vstpreset");
    std::vector<host::PresetRef> out;
    std::error_code ec;
    for (const auto& p : options.from) {
        if (fs::is_regular_file(p, ec)) {  // a file the user names is tried whatever its extension
            host::PresetRef r;
            r.method = PresetMethod::File;
            r.location = p.string();
            r.name = p.stem().string();
            out.push_back(std::move(r));
        } else {
            add_folder(p, options.raw_files, exts, out);
        }
    }
    if (options.standard_folders && info.format == host::PluginFormat::Vst3)
        for (const auto& d : vst3_preset_folders(info)) add_folder(d, false, {".vstpreset"}, out);
    return out;
}

struct PresetVerifier::Impl {
    host::Instance* inst = nullptr;
    host::Job plan;
    std::unique_ptr<opt::Embedder> embedder;
    Rendered def;
    std::string default_hash;
    double floor = 0, threshold = 0;
    struct Kept {
        std::string hash;
        int number = 0;
        Rendered render;
    };
    std::vector<Kept> kept;
    Rendered pending;  // the render of the last verified ok preset, kept by accept()

    Rendered render(const host::Job& job) {
        Rendered r;
        auto audio = inst->render(job);
        if (!audio) {
            r.why = std::format("{}{}", to_string(audio.error()), inst->last_message().empty() ? "" : ": " + inst->last_message());
            return r;
        }
        r.emb = embedder->embed(opt::mono_of(*audio));
        r.level = r.emb.size() > opt::kEmbeddingLevel ? r.emb[opt::kEmbeddingLevel] : 0.0;
        r.ok = true;
        return r;
    }
};

PresetVerifier::PresetVerifier() = default;
PresetVerifier::PresetVerifier(PresetVerifier&&) noexcept = default;
PresetVerifier& PresetVerifier::operator=(PresetVerifier&&) noexcept = default;
PresetVerifier::~PresetVerifier() = default;

expected<PresetVerifier, std::string> PresetVerifier::create(host::Instance& inst, const host::Job& job) {
    PresetVerifier v;
    v.impl_ = std::make_unique<Impl>();
    Impl& m = *v.impl_;
    m.inst = &inst;
    m.plan = job;
    m.plan.state.clear();
    m.plan.coords.clear();
    for (const auto& p : inst.parameters()) m.plan.coords.push_back(p.default_coord);
    if (m.plan.notes.empty()) m.plan.notes.push_back({48, 0, 100.0f / 127.0f, 0, 600});
    m.embedder = std::make_unique<opt::Embedder>(m.plan.sample_rate);
    // The defaults: reference sound, floor, and the state a preset must differ from.
    m.def = m.render(m.plan);
    const Rendered def2 = m.render(m.plan);
    if (!m.def.ok) return unexpected("the defaults do not render (" + m.def.why + ")");
    if (!def2.ok) return unexpected("the defaults render once but not twice (" + def2.why + ")");
    m.floor = difference(m.def, def2);
    m.threshold = std::max(2.0 * m.floor, 0.15);  // as the probe's "same sound" [D]
    if (auto st = inst.save_state()) m.default_hash = state_hash(*st);
    return v;
}

double PresetVerifier::floor() const { return impl_->floor; }
double PresetVerifier::threshold() const { return impl_->threshold; }

bool PresetVerifier::adopt(const HarvestEntry& e) {
    host::Job js = impl_->plan;
    js.coords.clear();
    js.state = e.captured.state;
    Rendered r = impl_->render(js);
    if (!r.ok) return false;
    impl_->kept.push_back({e.state_hash, e.number, std::move(r)});
    return true;
}

void PresetVerifier::verify(HarvestEntry& e, host::CapturedPreset& cap) {
    Impl& m = *impl_;
    e.state_hash = state_hash(cap.state);
    host::Job js = m.plan;
    js.coords.clear();
    js.state = cap.state;
    const auto dup = std::ranges::find_if(m.kept, [&](const Impl::Kept& k) { return k.hash == e.state_hash; });
    // A state equal to the defaults or to an earlier preset needs no render.
    const Rendered r1 = e.state_hash == m.default_hash || dup != m.kept.end() ? Rendered{} : m.render(js);
    e.difference = r1.ok ? difference(r1, m.def) : 0.0;
    if (e.state_hash == m.default_hash) {
        e.outcome = PresetOutcome::NoChange;
        e.message = "the state equals the defaults";
    } else if (dup != m.kept.end()) {
        e.outcome = PresetOutcome::Duplicate;
        e.duplicate_of = dup->number;
        e.message = "same state as an earlier preset";
    } else if (!r1.ok) {
        e.outcome = PresetOutcome::LoadError;
        e.message = "the loaded state does not render: " + r1.why;
    } else if (e.difference < m.threshold) {
        e.outcome = PresetOutcome::NoChange;
        e.message = std::format("sounds like the defaults ({:.3f} < {:.3f})", e.difference, m.threshold);
    } else {
        for (const auto& k : m.kept)
            if (difference(r1, k.render) < m.threshold) {
                e.outcome = PresetOutcome::Duplicate;
                e.duplicate_of = k.number;
                e.message = "sounds like an earlier preset";
                break;
            }
        if (e.duplicate_of == 0) {
            // The blob alone reproduces the sound: a second render after its own reset and load.
            const Rendered r2 = m.render(js);
            const double rep_diff = difference(r1, r2);
            if (rep_diff > m.threshold) {
                e.outcome = PresetOutcome::NotReproducible;
                e.message = r2.ok ? std::format("two renders of the state differ by {:.3f}", rep_diff)
                                  : "second render failed: " + r2.why;
            } else {
                e.outcome = PresetOutcome::Ok;
                host::Job jc = m.plan;
                jc.coords = cap.coords;
                e.params_reproduce = difference(m.render(jc), r1) < m.threshold;
                m.pending = r1;
            }
        }
    }
    if (e.outcome == PresetOutcome::Ok) e.captured = std::move(cap);
}

void PresetVerifier::accept(const HarvestEntry& e) {
    impl_->kept.push_back({e.state_hash, e.number, std::move(impl_->pending)});
    impl_->pending = {};
}

expected<HarvestResult, std::string> harvest(host::Instance& inst, const HarvestOptions& options) {
    HarvestResult result;
    auto made = PresetVerifier::create(inst, options.plan);
    if (!made) return unexpected(made.error());
    PresetVerifier verifier = std::move(*made);
    const host::Job plan = options.plan;
    result.floor = verifier.floor();
    result.threshold = verifier.threshold();
    const auto& params = inst.parameters();
    (void)params;

    // Candidates, grouped by method in ladder order (a recipe's method first).
    std::vector<host::PresetRef> refs = file_presets(options, inst.info());
    auto listed = inst.presets();
    if (listed) refs.insert(refs.end(), listed->begin(), listed->end());
    else result.listing_error = listed.error().message;  // reported, not dropped (rule 6)
    std::vector<PresetMethod> order;
    for (const auto& r : refs)
        if (std::ranges::find(order, r.method) == order.end()) order.push_back(r.method);
    std::ranges::stable_sort(order, [](PresetMethod a, PresetMethod b) { return ladder_rank(a) < ladder_rank(b); });
    if (options.recipe && !options.method) {
        const auto it = std::ranges::find(order, options.recipe->method);
        if (it != order.end()) std::rotate(order.begin(), it, it + 1);
    }
    if (options.method && std::ranges::find(order, *options.method) == order.end())
        return unexpected(std::format("the plugin offers no presets through '{}'{}", to_string(*options.method),
                                      listed ? "" : " (listing failed: " + listed.error().message + ")"));

    int ok_total = 0;
    // A resumed harvest: the earlier session's presets are taken over (ok ones re-rendered once, for the
    // duplicate check) and never loaded again.
    std::set<std::string> tried;
    std::map<PresetMethod, MethodReport> before;
    std::map<PresetMethod, host::PresetRef> before_first_ok;
    for (const auto& pe : options.prior) {
        HarvestEntry e = pe;
        tried.insert(ref_key(e.ref));
        auto& mr = before[e.ref.method];
        mr.method = e.ref.method;
        ++mr.tried;
        ++mr.resumed;
        if (e.outcome == PresetOutcome::Ok) {
            if (!verifier.adopt(e)) {  // the earlier state no longer renders: kept as a failure (spec 05), not tried again
                e.outcome = PresetOutcome::NotReproducible;
                e.message = "the state saved by the earlier session no longer renders";
                e.number = 0;
                e.captured = {};
                result.entries.push_back(std::move(e));
                continue;
            }
            ++mr.ok;
            if (!before_first_ok.count(e.ref.method)) before_first_ok[e.ref.method] = e.ref;
            ok_total = std::max(ok_total, e.number);
        }
        result.entries.push_back(std::move(e));
    }
    for (const PresetMethod method : order) {
        if (options.method && method != *options.method) continue;
        if (options.max_presets > 0 && ok_total >= options.max_presets) break;
        if (result.cancelled) break;
        const bool from_recipe = options.recipe && options.recipe->method == method;
        MethodReport rep;
        rep.method = method;
        std::vector<host::PresetRef> mine;
        for (const auto& r : refs)
            if (r.method == method) mine.push_back(r);
        // A recipe names the program parameter or list that worked: its presets first, the others after.
        if (from_recipe && (method == PresetMethod::ProgramList || method == PresetMethod::ProgramParam))
            std::ranges::stable_partition(mine, [&](const host::PresetRef& r) { return r.param_id == options.recipe->param_id; });
        std::optional<host::PresetRef> first_ok;  // the ref the recipe names
        rep.offered = static_cast<int>(mine.size());
        if (const auto it = before.find(method); it != before.end()) {
            rep.tried = it->second.tried;
            rep.ok = it->second.ok;
            rep.resumed = it->second.resumed;
            if (const auto f = before_first_ok.find(method); f != before_first_ok.end()) first_ok = f->second;
        }
        int empty_run = 0;
        for (const auto& ref : mine) {
            if (tried.count(ref_key(ref))) continue;  // done by an earlier session
            if (options.cancel && options.cancel()) {
                result.cancelled = true;
                rep.note = "cancelled";
                break;
            }
            if (options.max_presets > 0 && ok_total >= options.max_presets) {
                rep.note = std::format("stopped at --max {}", options.max_presets);
                break;
            }
            HarvestEntry e;
            e.ref = ref;
            ++rep.tried;
            e.position = rep.tried;
            e.of = rep.offered;
            auto cap = inst.apply_preset(ref, options.wait_ms);
            if (!cap) {
                e.message = cap.error().message;
                e.outcome = e.message.rfind("timeout", 0) == 0 ? PresetOutcome::Timeout : PresetOutcome::LoadError;
            } else {
                verifier.verify(e, *cap);
                e.name = !ref.name.empty() ? ref.name : cap->name;
            }
            if (e.outcome == PresetOutcome::Ok) {
                e.number = ++ok_total;
                if (e.name.empty()) e.name = std::format("{}-{:03}", to_string(method), e.number);
                verifier.accept(e);
                ++rep.ok;
                if (!first_ok) first_ok = ref;
            }
            const bool empty = e.outcome == PresetOutcome::NoChange || e.outcome == PresetOutcome::LoadError ||
                               e.outcome == PresetOutcome::Timeout;
            empty_run = empty ? empty_run + 1 : 0;
            if (options.progress) options.progress(e);
            result.entries.push_back(std::move(e));
            if (rep.ok == 0 && rep.tried == options.abandon_after && empty_run == rep.tried) {
                rep.note = std::format("abandoned: the first {} presets changed nothing or failed", rep.tried);
                break;
            }
            if (blind(method) && empty_run >= options.stop_after) {
                rep.note = std::format("end of the bank: {} empty slots in a row", empty_run);
                break;
            }
        }
        result.methods.push_back(rep);
        if (rep.ok > 0 && !result.recipe && first_ok) {
            opt::HarvestRecipe recipe;
            recipe.method = method;
            recipe.count = rep.offered;
            recipe.found = rep.ok;
            recipe.wait_ms = options.wait_ms;
            recipe.param_id = first_ok->param_id;
            recipe.list_id = first_ok->list_id;
            result.recipe = recipe;
        }
        if (result.cancelled) break;
        if (rep.ok > 0 && !options.all_methods && !options.method) break;  // each step runs until it yields presets
    }
    return result;
}

namespace {

std::string file_safe(const std::string& name) {
    std::string out;
    for (const char c : name) {
        const auto u = static_cast<unsigned char>(c);
        out += (std::isalnum(u) || c == '-' || c == '_' || c == '.' || c == ' ') ? c : '_';
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
    return out.empty() ? "preset" : out;
}

std::string csv(const std::string& s) {
    std::string out = "\"";
    for (const char c : s) out += c == '"' ? std::string("\"\"") : std::string(1, c);
    return out + "\"";
}

}  // namespace

expected<fs::path, std::string> write_library(const fs::path& root, const HarvestResult& result, const host::PluginInfo& info,
                                              const std::vector<host::ParamDesc>& params, const output::ValueText& text) {
    const fs::path dir = opt::plugin_file_path(root, info, "");
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) return unexpected("cannot create " + dir.string());
    // A re-harvest replaces the library: the files the previous index listed go, nothing else in the folder.
    // Entries taken over from a resumed harvest keep theirs.
    std::set<std::string> keep;
    for (const auto& e : result.entries) {
        if (!e.state_file.empty()) keep.insert(e.state_file);
        if (!e.params_file.empty()) keep.insert(e.params_file);
    }
    if (std::ifstream old(dir / "index.json", std::ios::binary); old) {
        std::stringstream ss;
        ss << old.rdbuf();
        if (auto v = json::parse(ss.str()))
            for (const auto& e : (*v)["presets"].as_array())
                for (const char* key : {"state", "params"}) {
                    const std::string name = e[key].as_string();
                    if (!name.empty() && !keep.count(name) && fs::path(name).filename() == fs::path(name)) fs::remove(dir / name, ec);
                }
    }

    json::Array presets;
    std::string report = "number,name,method,index,outcome,duplicate_of,difference,params_reproduce,state_hash,message\n";
    for (const auto& e : result.entries) {
        json::Value v;
        v.set("number", e.number).set("name", e.name).set("method", std::string(host::to_string(e.ref.method)))
            .set("ref", host::to_json(e.ref)).set("outcome", std::string(to_string(e.outcome))).set("message", e.message)
            .set("state_hash", e.state_hash).set("duplicate_of", e.duplicate_of).set("difference", e.difference)
            .set("params_reproduce", e.params_reproduce);
        if (e.outcome == PresetOutcome::Ok && !e.state_file.empty()) {
            v.set("state", e.state_file).set("params", e.params_file);  // written by an earlier session
        } else if (e.outcome == PresetOutcome::Ok) {
            const std::string stem = std::format("{:03}-{}", e.number, file_safe(e.name));
            if (auto w = output::write_bytes(dir / (stem + ".state"), e.captured.state); !w) return unexpected(w.error());
            auto pj = output::params_json(info, params, e.captured.coords, text);
            pj.set("preset", e.name).set("method", std::string(host::to_string(e.ref.method)));
            if (auto w = output::write_text(dir / (stem + ".params.json"), pj.dump(2) + "\n"); !w) return unexpected(w.error());
            v.set("state", stem + ".state").set("params", stem + ".params.json");
        }
        presets.push_back(std::move(v));
        report += std::format("{},{},{},{},{},{},{:.4f},{},{},{}\n", e.number, csv(e.name), host::to_string(e.ref.method), e.ref.index,
                              to_string(e.outcome), e.duplicate_of, e.difference, e.params_reproduce ? "yes" : "no", e.state_hash,
                              csv(e.message));
    }
    json::Array methods;
    for (const auto& m : result.methods) {
        json::Value v;
        v.set("method", std::string(host::to_string(m.method))).set("offered", m.offered).set("tried", m.tried).set("ok", m.ok)
            .set("note", m.note);
        methods.push_back(std::move(v));
    }
    if (result.cancelled) report += "# cancelled: continue with --continue\n";
    json::Value index;
    index.set("cancelled", result.cancelled);
    index.set("format", std::string(host::to_string(info.format))).set("plugin", info.name).set("vendor", info.vendor)
        .set("uid", info.uid).set("version_hash", info.version_hash).set("floor", result.floor).set("threshold", result.threshold)
        .set("methods", std::move(methods)).set("presets", std::move(presets));
    if (!result.listing_error.empty()) index.set("listing_error", result.listing_error);
    if (result.recipe) index.set("recipe", result.recipe->to_json());
    if (auto w = output::write_text(dir / "index.json", index.dump(2) + "\n"); !w) return unexpected(w.error());
    if (auto w = output::write_text(dir / "harvest-report.csv", report); !w) return unexpected(w.error());
    return dir;
}

}  // namespace mimicry::targets

namespace mimicry::targets {

expected<std::vector<HarvestEntry>, std::string> load_library(const fs::path& dir) {
    std::ifstream f(dir / "index.json", std::ios::binary);
    if (!f) return unexpected("no library at " + dir.string());
    std::stringstream ss;
    ss << f.rdbuf();
    auto v = json::parse(ss.str());
    if (!v) return unexpected((dir / "index.json").string() + ": " + v.error());
    std::vector<HarvestEntry> out;
    for (const auto& p : (*v)["presets"].as_array()) {
        HarvestEntry e;
        e.number = int(p["number"].as_number());
        e.name = p["name"].as_string();
        if (auto ref = host::preset_from_json(p["ref"])) e.ref = *ref;
        else continue;  // an entry without a usable reference cannot be skipped later; it is tried again
        e.outcome = parse_outcome_name(p["outcome"].as_string());
        e.message = p["message"].as_string();
        e.state_hash = p["state_hash"].as_string();
        e.duplicate_of = int(p["duplicate_of"].as_number());
        e.difference = p["difference"].as_number();
        e.params_reproduce = p["params_reproduce"].as_bool();
        if (e.outcome == PresetOutcome::Ok) {
            const std::string sf = p["state"].as_string();
            if (sf.empty() || fs::path(sf).filename() != fs::path(sf)) continue;
            std::ifstream sfile(dir / sf, std::ios::binary);
            if (!sfile) continue;  // the state file is gone: the preset is harvested again
            e.captured.state.assign(std::istreambuf_iterator<char>(sfile), std::istreambuf_iterator<char>());
            e.captured.name = e.name;
            e.state_file = sf;
            e.params_file = p["params"].as_string();
        }
        out.push_back(std::move(e));
    }
    return out;
}

}  // namespace mimicry::targets
