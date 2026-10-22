// mimicry: supervisor / command line (docs/spec/01_ARCHITECTURE.md section 6).
// Implemented: list, probe, fit, fingerprint, harvest, replay. The rest are implemented in the order of docs/PROJECT_PLAN.md.
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

#include "app/Cli.hpp"
#include "app/Commands.hpp"
#include "app/Console.hpp"
#include "app/RunDir.hpp"
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
#include "opt/PitchCalibration.hpp"
#include "opt/Probe.hpp"
#include "opt/Profile.hpp"
#include "opt/Space.hpp"
#include "output/FailureReport.hpp"
#include "output/PresetExport.hpp"
#include "supervisor/WorkerClient.hpp"
#include "supervisor/WorkerPool.hpp"
#include "targets/Harvest.hpp"

namespace {

using namespace mimicry;
using namespace mimicry::app;
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
    {"harvest", "harvest <plugin>", "collect the plugin's presets as verified states (preset library)"},
    {"transfer", "transfer --source A --target B", "match the source plugin's presets in another plugin (any format mix)"},
    {"runs", "runs", "list the run directories with status (resumable runs)"},
    {"cancel", "cancel <run dir>", "ask a running fit or transfer to stop and keep a checkpoint"},
    {"image", "image --plugin P --image I.png", "match an image"},
    {"fingerprint", "fingerprint <plugin>", "build the random-preset feature database"},
    {"rehearse", "rehearse <plugin>", "round-trip self-test"},
    {"eval", "eval --plugin P --state S --wav T.wav", "score a state (or .vstpreset) without fitting"},
    {"replay", "replay <crash.json>", "re-run a recorded failure (in-process unless --isolated)"},
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
  --settle-ms MS       silence after the parameters, before the notes (default 250, or the profile's)
  --velocity V         note velocity 1-127 (default 100)
  --timeout-min S      watchdog per render: at least S seconds (5) and --timeout-factor F x the audio length (20)

machine-readable output and control (docs/spec/09_GUI.md):
  --events             stdout is one JSON object per line (start, phase, progress, stage, preset, log, done)
  --cancel-on-eof      with --events: the end of stdin cancels the run (a front end that died)
  Ctrl-C, SIGTERM, a line "cancel" on stdin (with --events), or `mimicry cancel <run>` stop a run
  cleanly: results so far are written and the run can continue later. Exit codes: 0 done, 1 failed,
  4 cancelled (resumable).

fit options:
  --minutes M          time cap (default 20)        --patience S   seconds without improvement (240)
  --evals N            evaluation budget            --mode hybrid|guided|unsupervised
  --only a,b / --fix a,b   parameter names or ids   --stages 1,5,7
  --restarts R (3)     --top-k K (3)                --seed N        --no-prior
  --kind note|hit|texture|loop                      --w-<feature> N
  --solve-gain         closed-form gain even with a level parameter
  --out DIR            run directory root (default: runs)
  --resume RUN         continue a cancelled or interrupted run (its options are kept; others override)
  --more-minutes M     with --resume: M more minutes (default: what is left of the time cap)
  --progress-s S       seconds between progress reports (1 with --events, else 15)

transfer options (fit options apply per preset; --minutes defaults to 5 per preset):
  --library DIR        the source's preset library (default: presets/<source>, harvested if missing)
  --from F,G / --method M   harvest options for that on-demand harvest      --no-harvest   never harvest
  --preset NAME|N,...  only these presets      --max N   the first N presets      --total-minutes T   overall cap
  --note C3 --gate MS --velocity V   the note both plugins play                 --resume RUN

harvest options:
  --from F,G           preset files or folders (.state, .vstpreset; --raw-files: any file as state)
  --method M           only file|clap-preset|program-data|program-list|midi-program|program-param
  --all-methods        try every method (default: stop at the first that yields presets)
  --max N              presets to collect    --wait-ms MS (200) after each load
  --no-recipe          ignore the profile's recipe    --out DIR library root (default: presets)
  --continue           skip what an earlier (cancelled) harvest of this plugin already tried

probe options: --no-pairs      fingerprint options: --count N (2000)
replay options: --plugin P (default: the path in the report)   --isolated (render in a worker)

eval options: --state S (best.state or .vstpreset)   --json   --out FILE
runs options: --out DIR (default: runs)   --json

<plugin> is a .clap or .vst3 path. image and rehearse are not implemented yet;
see docs/PROJECT_PLAN.md for the order.
)";
    std::fputs(text.c_str(), stdout);
}

int not_implemented(std::string_view command) {
    std::fputs(std::format("mimicry {}: not implemented yet (see docs/PROJECT_PLAN.md)\n", command).c_str(), stderr);
    return 2;
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
    out += std::format("{:>8}  {:<24} {:<14} {:<6} {:>5}  {:<12} {:<22} {:<12} {}\n", "id", "name", "module", "units", "steps",
                       "group", "param role", "search", "flags");
    for (std::size_t i = 0; i < p.params.size(); ++i) {
        const auto& d = p.params[i];
        const auto& s = space.slot(i);
        const auto pr = s.param_role.role == ParamRole::None ? std::string("-")
                                                             : std::format("{} {:.2f}", to_string(s.param_role.role), s.param_role.confidence);
        out += std::format("{:>8}  {:<24} {:<14} {:<6} {:>5}  {:<6} {:.2f}  {:<22} {:<12} {}{}\n", d.id, d.name, d.module, d.units,
                           d.steps, to_string(s.group), s.names.confidence, pr, to_string(s.role), flags_text(d.flags),
                           s.why.empty() ? "" : "(" + s.why + ")");
    }
    Console::get().info(out);
    Console::get().done("finished");
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
    po.job.settle_ms = a.num("--settle-ms", po.job.settle_ms);
    po.job.tail_max_ms = 3000;
    po.pairs = !a.has("--no-pairs");
    po.log = [](const std::string& s) { Console::get().info(s); };
    Console::get().info(std::format("probing {} parameters with {} worker(s)...", p.params.size(), p.workers()));
    if (Console::get().events()) Console::get().emit(json::Value().set("event", "phase").set("name", "probing").set("parameters", p.params.size()));
    auto prof = opt::probe(p.batch(), p.info, p.params, po);
    if (!prof) return fail("probe", prof.error());
    if (p.pool && !p.pool->table_change().empty())
        return fail("probe", "the plugin changed its parameter table during the probe (" + p.pool->table_change() +
                                 "); the probe is not valid for either table");
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
    if (const auto old = load_profile(a, p.info)) {  // learned by earlier fits and harvests
        prof->constraints = old->constraints;
        prof->recipe = old->recipe;
    }
    if (auto r = prof->save(path); !r) return fail("probe", r.error());
    out += "wrote " + path.string();
    Console::get().info(out);
    Console::get().done("finished", json::Value().set("profile", path.string()));
    return 0;
}

int cmd_fingerprint(const Args& a) {
    if (a.positional.empty()) return fail("fingerprint", "usage: mimicry fingerprint <plugin>");
    auto plugin = open_plugin(a.positional[0], a);
    if (!plugin) return fail("fingerprint", plugin.error());
    auto& p = **plugin;
    const auto profile = load_profile(a, p.info);
    if (!profile || !profile->probed())
        Console::get().warn("no profile; run `mimicry probe` first for a safe box and dead parameters");
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
        if (std::chrono::steady_clock::now() - last < std::chrono::seconds(Console::get().events() ? 1 : 15) && done < total) return;
        last = std::chrono::steady_clock::now();
        if (Console::get().events()) {
            Console::get().emit(json::Value().set("event", "progress").set("stage", "fingerprint").set("evals", static_cast<std::int64_t>(done))
                                    .set("fraction", total > 0 ? double(done) / double(total) : 1.0));
            return;
        }
        Console::get().info(std::format("  {} / {}", done, total));
    };
    const auto db = opt::build_fingerprints(p.batch(), p.info, space, fo);
    const auto path = opt::FingerprintDb::default_path(profiles_dir(a), p.info);
    if (auto r = db.save(path); !r) return fail("fingerprint", r.error());
    Console::get().info(std::format("{} valid of {} rendered; wrote {}", db.size(), db.rendered, path.string()));
    Console::get().done("finished", json::Value().set("fingerprints", path.string()));
    return 0;
}

int cmd_replay(const Args& a) {
    if (a.positional.empty()) return fail("replay", "usage: mimicry replay <crash.json> [--plugin P] [--isolated]");
    auto report = output::FailureReport::load(a.positional[0]);
    if (!report) return fail("replay", report.error());
    const std::string path = a.get("--plugin", report->plugin_path);
    if (path.empty()) return fail("replay", "the report names no plugin path; pass --plugin");
    std::unique_ptr<host::Instance> inst;
    if (a.has("--isolated")) {
        auto c = supervisor::WorkerClient::open(path, report->info.uid);
        if (!c) return fail("replay", c.error().message);
        inst = std::move(*c);
    } else {
        auto c = host::open(path, report->info.uid);
        if (!c) return fail("replay", c.error().message);
        inst = std::move(*c);
    }
    const auto& info = inst->info();
    if (info.format != report->info.format || info.uid != report->info.uid)
        return fail("replay", std::format("{} is {} {}, the report is for {} {}", path, host::to_string(info.format), info.uid,
                                          host::to_string(report->info.format), report->info.uid));
    if (info.version_hash != report->info.version_hash)
        std::fputs(std::format("note: plugin version {} differs from the report's {}\n", info.version_hash,
                               report->info.version_hash).c_str(), stderr);
    std::vector<std::string> missing;
    host::Job job = report->job;
    job.coords = report->coords_for(inst->parameters(), &missing);
    for (const auto& m : missing) std::fputs(std::format("note: parameter {} is not in this build\n", m).c_str(), stderr);
    std::string s = std::format("replaying {} on {} {} ({}){}\n", to_string(report->outcome), host::to_string(info.format),
                                info.name, a.has("--isolated") ? "worker" : "in-process", report->message.empty() ? "" : ": " + report->message);
    for (const auto& m : report->minimal) s += std::format("  minimal set: {} (id {}) threshold {:.4f}\n", m.name, m.id, m.threshold);
    std::fputs(s.c_str(), stdout);
    std::fflush(stdout);
    const auto r = inst->render(job);
    const Outcome got = r ? Outcome::Ok : r.error();
    const bool same = got == report->outcome;
    std::fputs(std::format("result: {}{} -> {}\n", to_string(got), inst->last_message().empty() ? "" : " (" + inst->last_message() + ")",
                           same ? "reproduced" : "NOT reproduced").c_str(), stdout);
    return same ? 0 : 3;
}

}  // namespace

int main(int argc, char** argv) {
    using namespace mimicry::app;
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
    Console& console = Console::get();
    console.set_events(args.has("--events"));
    console.install_signal_handlers();
    if (console.events()) console.start_stdin_watcher(args.has("--cancel-on-eof"));
    if (cmd == "fit") return cmd_fit(args);
    if (cmd == "eval") return cmd_eval(args);
    if (cmd == "runs") return cmd_runs(args);
    if (cmd == "cancel") return cmd_cancel(args);
    if (cmd == "transfer") return cmd_transfer(args);
    if (cmd == "list") return cmd_list(args);
    if (cmd == "probe") return cmd_probe(args);
    if (cmd == "fingerprint") return cmd_fingerprint(args);
    if (cmd == "replay") return cmd_replay(args);
    if (cmd == "harvest") return mimicry::app::cmd_harvest(args);
    for (const auto& c : kCommands)
        if (c.name == cmd) return not_implemented(cmd);
    std::fputs(std::format("mimicry: unknown command '{}' (try --help)\n", cmd).c_str(), stderr);
    return 1;
}
