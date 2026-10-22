// mimicry fit / eval / runs / cancel (docs/spec/09_GUI.md sections 2-3).

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <format>
#include <fstream>

#include "app/Commands.hpp"
#include "app/Console.hpp"
#include "app/FitRun.hpp"
#include "app/RunDir.hpp"
#include "core/Wav.hpp"
#include "features/Loss.hpp"
#include "features/Pitch.hpp"
#include "opt/Pipeline.hpp"
#include "opt/PitchCalibration.hpp"
#include "output/PresetExport.hpp"
#include "output/VstPreset.hpp"

namespace mimicry::app {

namespace fs = std::filesystem;

namespace {

// The numbers a run.json carries for lists and the GUI.
json::Value run_numbers(const FitOutcome& o) {
    json::Value v;
    v.set("evals", static_cast<std::int64_t>(o.evals)).set("elapsed_s", o.seconds).set("time_cap_s", o.time_cap_s);
    if (o.have_best) v.set("loss_db", o.loss_db).set("gain_db", o.gain_db);
    return v;
}

}  // namespace

int cmd_fit(const Args& cli) {
    Console& con = Console::get();
    Args a = cli;
    std::optional<RunDir> run;
    bool resume = false;
    if (cli.has("--resume")) {
        auto r = RunDir::open(cli.get("--resume"));
        if (!r) return fail("fit", r.error());
        if (r->meta()["kind"].as_string() != "fit") return fail("fit", cli.get("--resume") + " is not a fit run");
        a = r->saved_args().merged_with(without_control_options(cli));
        run = std::move(*r);
        resume = true;
        std::error_code ec;
        fs::remove(run->cancel_path(), ec);  // the cancel that stopped the last session
    }
    const std::string plugin_path = a.get("--plugin"), wav_path = a.get("--wav");
    if (plugin_path.empty() || wav_path.empty())
        return fail("fit", "usage: mimicry fit --plugin P --wav T.wav   |   mimicry fit --resume <run dir>");
    // The run directory first, so the first event names it and every later failure is recorded in it.
    if (!run) {
        auto r = RunDir::create(a.get("--out", "runs"), "fit", fs::path(plugin_path).stem().string() + "-" + fs::path(wav_path).stem().string(),
                                without_control_options(a));
        if (!r) return fail("fit", r.error());
        run = std::move(*r);
    }
    run->update(json::Value().set("kind", "fit").set("plugin", plugin_path).set("target", wav_path));
    run->set_status("running");
    con.watch_cancel_file(run->cancel_path());
    if (con.events()) {
        json::Value s;
        s.set("event", "start").set("command", "fit").set("run_dir", run->path().string()).set("plugin", plugin_path)
            .set("target", wav_path).set("resumed", resume);
        con.emit(std::move(s));
        con.emit(json::Value().set("event", "phase").set("name", "analysing the target"));
    }
    auto die = [&](const std::string& message) {
        run->set_status("failed", json::Value().set("message", message));
        return fail("fit", message);
    };
    auto wav = read_wav(wav_path);
    if (!wav) return die(wav.error());
    features::AnalyzeOptions ao;
    if (a.has("--note")) {
        ao.key = features::parse_note_name(a.get("--note"));
        if (!ao.key) return die("bad --note");
    }
    if (a.has("--kind")) {
        ao.kind = features::parse_kind(a.get("--kind"));
        if (!ao.kind) return die("bad --kind");
    }
    auto target = features::Target::from_wav(*wav, ao);
    if (!target) return die(target.error());
    const auto& an = target->analysis();
    con.info(std::format("target: {}, f0 {:.2f} Hz (confidence {:.2f}), key {} {:+.1f} cents, {:.0f} ms", features::to_string(an.kind), an.f0,
                         an.confidence, an.key, an.cents, an.duration_ms));
    if (con.events()) con.emit(json::Value().set("event", "phase").set("name", "opening the plugin"));

    auto plugin = open_plugin(plugin_path, a);
    if (!plugin) return die(plugin.error());
    FitSpec sp;
    sp.plugin = plugin->get();
    sp.args = &a;
    sp.target = &*target;
    sp.analyze = ao;
    sp.dir = run->path();
    sp.target_label = wav_path;
    sp.resume = resume;
    sp.more_minutes = cli.num("--more-minutes", 0);
    sp.on_progress = [&](const opt::Progress& pr) {
        run->update(json::Value().set("evals", static_cast<std::int64_t>(pr.evals)).set("elapsed_s", pr.seconds).set("loss_db", pr.best)
                        .set("stage", pr.stage));
    };
    const FitOutcome out = run_fit(sp);
    json::Value numbers = run_numbers(out);
    if (out.status == "failed") {
        numbers.set("message", out.error);
        run->set_status("failed", numbers);
        return fail("fit", out.error);
    }
    run->set_status(out.status, numbers);
    if (con.events()) {
        json::Value d = numbers;
        d.set("run_dir", run->path().string()).set("resumable", out.status == "cancelled");
        if (out.have_best) d.set("files", json::Value().set("ab", (run->path() / "ab.wav").string()).set("match", (run->path() / "match.wav").string())
                                              .set("report", (run->path() / "report.md").string())
                                              .set("spectrogram", (run->path() / "diff.bmp").string()));
        con.done(out.status, std::move(d));
    } else if (out.status == "cancelled") {
        con.info(std::format("cancelled; continue with: mimicry fit --resume {}", run->path().string()));
    }
    return out.status == "cancelled" ? kExitCancelled : 0;
}

// mimicry eval --plugin P --state S --wav T.wav: scores a state (or .vstpreset) without fitting [S: --evaluate-only].
int cmd_eval(const Args& a) {
    Console& con = Console::get();
    const std::string plugin_path = a.get("--plugin"), wav_path = a.get("--wav"), state_path = a.get("--state");
    if (plugin_path.empty() || wav_path.empty() || state_path.empty())
        return fail("eval", "usage: mimicry eval --plugin P --state S --wav T.wav   (S: best.state or a .vstpreset)");
    auto wav = read_wav(wav_path);
    if (!wav) return fail("eval", wav.error());
    features::AnalyzeOptions ao;
    if (a.has("--note")) {
        ao.key = features::parse_note_name(a.get("--note"));
        if (!ao.key) return fail("eval", "bad --note");
    }
    if (a.has("--kind")) {
        ao.kind = features::parse_kind(a.get("--kind"));
        if (!ao.kind) return fail("eval", "bad --kind");
    }
    auto target = features::Target::from_wav(*wav, ao);
    if (!target) return fail("eval", target.error());
    const auto& an = target->analysis();

    std::ifstream f(state_path, std::ios::binary);
    if (!f) return fail("eval", "cannot read " + state_path);
    std::vector<std::uint8_t> state((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

    auto plugin = open_plugin(plugin_path, a);
    if (!plugin) return fail("eval", plugin.error());
    Plugin& p = **plugin;
    if (fs::path(state_path).extension() == ".vstpreset") {
        auto parsed = output::parse_vstpreset(state);
        if (!parsed) return fail("eval", parsed.error());
        if (p.info.format != host::PluginFormat::Vst3 || parsed->class_id != p.info.uid)
            return fail("eval", std::format("{} is a preset for class {}, not for this plugin", state_path, parsed->class_id));
        state = output::vst3_state_from_chunks(parsed->chunks);
    }
    const auto stored = load_profile(a, p.info);
    const bool pitched = an.f0 > 0 && an.kind == features::SampleKind::Note;
    host::Job job = note_job(a, pitched ? an.key : 60, pitched ? an.cents : 0.0, std::max(50.0, an.duration_ms));
    job.sample_rate = ao.sample_rate;
    if (stored && stored->settle_ms * 1.5 > job.settle_ms) job.settle_ms = stored->settle_ms * 1.5;
    job.settle_ms = a.num("--settle-ms", job.settle_ms);
    if (pitched && stored && stored->pitch.method != opt::PitchMethod::Unknown) opt::apply_pitch_method(stored->pitch, job);
    job.state = state;
    auto r = p.batch()({job});
    if (r.empty() || !r.front()) return fail("eval", std::format("the state does not render ({})", r.empty() ? "no result" : std::string(to_string(r.front().error()))));
    auto weights = features::Weights::preset(an.kind);
    for (const auto& [k, v] : a.opts)
        if (k.rfind("--w-", 0) == 0 && !weights.set(k.substr(4), a.num(k, 0))) return fail("eval", "unknown weight " + k);
    features::ScorerOptions sco;
    sco.solve_gain = true;
    features::Scorer scorer(*target, weights, sco);
    const auto mono = opt::mono_of(*r.front());
    scorer.set_offset(scorer.estimate_offset(mono));
    const auto score = scorer.score(mono);
    json::Value out;
    out.set("loss_db", score.cost).set("gain_db", score.gain_db).set("offset_samples", static_cast<std::int64_t>(scorer.offset()));
    json::Object per;
    for (std::size_t i = 0; i < features::kFeatureCount; ++i) per[std::string(features::to_string(features::Feature(i)))] = score.loss[i];
    out.set("features", json::Value(std::move(per)));
    if (a.has("--out")) {
        if (auto w = output::write_text(a.get("--out"), out.dump(2) + "\n"); !w) return fail("eval", w.error());
    }
    if (con.events()) {
        con.done("finished", out);
        return 0;
    }
    if (a.has("--json")) {
        std::fputs((out.dump() + "\n").c_str(), stdout);
        return 0;
    }
    std::string text = std::format("{:.3f} dB (gain {:+.1f} dB, onset offset {} samples)\n", score.cost, score.gain_db, scorer.offset());
    for (std::size_t i = 0; i < features::kFeatureCount; ++i)
        text += std::format("  {:<8} {:8.3f} dB  (weight {:.2f})\n", features::to_string(features::Feature(i)), score.loss[i], weights.w[i]);
    con.info(text);
    return 0;
}

// mimicry runs [--out runs] [--json]
int cmd_runs(const Args& a) {
    const auto runs = list_runs(a.get("--out", "runs"));
    if (a.has("--json")) {
        json::Array arr;
        for (const auto& r : runs) {
            json::Value m = r.meta;
            m.set("dir", r.dir.string()).set("resumable", r.resumable);
            arr.push_back(std::move(m));
        }
        std::fputs((json::Value(std::move(arr)).dump(2) + "\n").c_str(), stdout);
        return 0;
    }
    if (runs.empty()) {
        std::puts("no runs");
        return 0;
    }
    for (const auto& r : runs)
        std::fputs(std::format("{:<10} {:<8} {:>9.3f} dB {:>8} evals {:>7.0f} s  {}{}\n", r.status, r.kind, r.loss_db, r.evals, r.elapsed_s,
                               r.dir.string(), r.resumable ? "  (resumable)" : "").c_str(), stdout);
    return 0;
}

// mimicry cancel <run dir>: asks a running fit or transfer to stop and keep a checkpoint.
int cmd_cancel(const Args& a) {
    if (a.positional.empty()) return fail("cancel", "usage: mimicry cancel <run dir>");
    auto r = RunDir::open(a.positional[0]);
    if (!r) return fail("cancel", r.error());
    std::ofstream f(r->cancel_path());
    if (!f) return fail("cancel", "cannot write " + r->cancel_path().string());
    f << "cancel\n";
    Console::get().info("cancel requested for " + r->path().string());
    return 0;
}

}  // namespace mimicry::app
