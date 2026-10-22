#include "app/FitRun.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <format>

#include "app/Console.hpp"
#include "core/Wav.hpp"
#include "features/Pitch.hpp"
#include "host/Codec.hpp"
#include "opt/Fingerprint.hpp"
#include "opt/Objective.hpp"
#include "opt/PitchCalibration.hpp"
#include "opt/Space.hpp"
#include "output/FailureReport.hpp"
#include "output/Ppm.hpp"
#include "output/PresetExport.hpp"
#include "output/VstPreset.hpp"
#include "supervisor/WorkerClient.hpp"

namespace mimicry::app {

namespace fs = std::filesystem;

namespace {

struct Written {
    bool state = false, vstpreset = false;
};

// report.md (docs/spec/07_OUTPUT_VALIDATION.md section 1): what a person reads first.
std::string report_markdown(const FitSpec& sp, const FitOutcome& out, const opt::Space& space, const std::vector<host::ParamDesc>& params,
                            const host::PluginInfo& info, const features::TargetAnalysis& an, const std::string& status,
                            double floor_db, const std::vector<std::string>& at_bound, int sessions,
                            const opt::ConstraintSet& constraints, const std::string& pitch_line) {
    const auto& res = out.result;
    std::string md = std::format("# {} on {}\n\n", info.name, sp.target_label);
    md += std::format("- status: **{}**{}\n", status, sessions > 1 ? std::format(" (resumed {} time(s))", sessions - 1) : std::string());
    md += std::format("- plugin: {} {} ({}), uid {}, version {}\n", host::to_string(info.format), info.name, info.vendor, info.uid, info.version_hash);
    md += std::format("- target: {}, f0 {:.2f} Hz, {:.0f} ms\n", features::to_string(an.kind), an.f0, an.duration_ms);
    md += std::format("- result: **{:.3f} dB** (gain {:+.1f} dB) after {} evaluations in {:.0f} s\n", out.loss_db, out.gain_db, res.evals, res.seconds);
    if (floor_db > 0) md += std::format("- repeatability floor: {:.3f} dB (a loss near it cannot be told from the plugin's own noise)\n", floor_db);
    if (!pitch_line.empty()) md += "- pitch: " + pitch_line + "\n";
    if (res.surrogate_used) md += "- the surrogate pre-screened candidates (slow plugin)\n";
    md += "\nListen to `ab.wav` (target, silence, match): the score smooths over narrow features that matter by ear.\n";
    md += "\n## Stages\n\n| stage | free | before dB | after dB | evals | seconds |\n| --- | ---: | ---: | ---: | ---: | ---: |\n";
    for (const auto& s : res.stages)
        md += std::format("| {} | {} | {:.3f} | {:.3f} | {} | {:.0f} |\n", s.name, s.free, s.before, s.after, s.evals, s.seconds);
    md += "\n## Features of the best candidate\n\n| feature | loss dB |\n| --- | ---: |\n";
    for (std::size_t f = 0; f < features::kFeatureCount; ++f)
        md += std::format("| {} | {:.3f} |\n", features::to_string(features::Feature(f)), res.best.eval.score.loss[f]);
    if (!at_bound.empty()) {
        md += "\n## Parameters at a bound\n\nThese usually mean the plugin cannot reach the target, or compensates for something else.\n\n";
        for (const auto& t : at_bound) md += "- " + t + "\n";
    }
    md += "\n## Outcomes\n\n";
    for (const auto& [o, n] : res.outcomes) md += std::format("- {}: {}\n", to_string(o), n);
    if (!res.learned.empty() || !constraints.empty()) {
        md += std::format("\n## Hazards\n\n{} constraint(s) known, {} learned in this session; {} sample(s) redrawn, {} seed point(s) skipped.\n\n",
                          constraints.constraints().size(), res.learned.size(), res.resampled, res.skipped);
        for (const auto& c : res.learned) md += "- " + c.describe() + "\n";
    }
    if (!res.failures.empty()) md += std::format("\n{} failure report(s) are in `crashes/` (`mimicry replay <file>`).\n", res.failures.size());
    (void)space;
    (void)params;
    return md;
}

}  // namespace

FitOutcome run_fit(const FitSpec& sp) {
    FitOutcome out;
    Console& con = Console::get();
    Plugin& p = *sp.plugin;
    const Args& a = *sp.args;
    const features::Target& target = *sp.target;
    const auto& an = target.analysis();
    const fs::path dir = sp.dir;
    auto fail = [&](std::string msg) {
        out.status = "failed";
        out.error = std::move(msg);
        return out;
    };

    const auto stored = load_profile(a, p.info);
    // A profile written by an earlier fit before any probe carries only constraints.
    const auto profile = stored && stored->probed() ? stored : std::nullopt;
    if (!profile) con.warn("no plugin profile; `mimicry probe` first makes the fit faster and safer");
    opt::ConstraintSet constraints(p.params);
    if (stored) {
        const auto n = constraints.adopt(stored->constraints);
        if (n) con.info(std::format("profile: {} hazard constraint(s)", n));
    }

    // S0: the target's pitch is applied to the note (spec 04 section 4), so pitch-only parameters are pinned.
    const bool pitched = an.f0 > 0 && an.kind == features::SampleKind::Note;
    host::Job job = note_job(a, pitched ? an.key : 60, pitched ? an.cents : 0.0, std::max(50.0, an.duration_ms));
    job.sample_rate = sp.analyze.sample_rate;
    // Settle (spec 05 section 6): 1.5 x the probe's measurement when that is longer than the default [D];
    // --settle-ms decides outright.
    if (profile && profile->settle_ms * 1.5 > job.settle_ms) job.settle_ms = profile->settle_ms * 1.5;
    job.settle_ms = a.num("--settle-ms", job.settle_ms);
    opt::SpaceOptions so;
    const std::string mode = a.get("--mode", "hybrid");
    so.mode = mode == "guided" ? opt::Mode::Guided : mode == "unsupervised" ? opt::Mode::Unsupervised : opt::Mode::Hybrid;
    so.only = split(a.get("--only"));
    so.fix = split(a.get("--fix"));
    so.pin_pitch = pitched;
    const auto space = opt::Space::build(p.params, p.info.format, profile ? &*profile : nullptr, so);

    // How the plugin takes the cents (spec 02 section 5): from the profile, else three calibration renders.
    opt::PitchCalibration pitch;
    bool pitch_measured = false;
    std::string pitch_line;
    if (pitched) {
        if (stored && stored->pitch.method != opt::PitchMethod::Unknown) {
            pitch = stored->pitch;
        } else {
            host::Job cj = job;
            cj.coords = space.defaults();
            pitch = opt::calibrate_pitch(p.batch(), cj);
            pitch_measured = pitch.method != opt::PitchMethod::Unknown;
        }
        opt::apply_pitch_method(pitch, job);
        pitch_line = std::string(to_string(pitch.method)) +
                     (pitch.method == opt::PitchMethod::Bend ? std::format(" (bend range {:.2f} semitones)", pitch.bend_range)
                      : pitch.method == opt::PitchMethod::NearestKey ? std::format(" ({:+.1f} cents cannot be played)", an.cents)
                                                                      : std::string());
        con.info("pitch: " + pitch_line);
    }

    auto weights = features::Weights::preset(an.kind);
    for (const auto& [k, v] : a.opts)
        if (k.rfind("--w-", 0) == 0 && !weights.set(k.substr(4), a.num(k, 0))) return fail("unknown weight " + k);
    features::ScorerOptions sco;
    sco.solve_gain = a.has("--solve-gain") || space.level_index() < 0;
    opt::AudioObjective obj(p.batch(), job, target, weights, sco, p.workers());
    if (profile) {
        const auto rp = features::repeat_policy(profile->floor_db);
        obj.set_renders_per_candidate(rp.renders);
        out.floor_db = profile->floor_db;
    }
    if (!obj.calibrate_offset(space.defaults())) con.warn("the defaults do not render; no onset offset fitted");

    std::vector<std::vector<double>> seeds = sp.seeds;
    if (auto db = opt::FingerprintDb::load(opt::FingerprintDb::default_path(profiles_dir(a), p.info)); db) {
        if (db->matches(p.info, p.params)) {
            opt::Embedder em(job.sample_rate);
            std::size_t n = 0;
            for (auto i : db->nearest(em.embed(target.mono()), 8)) {
                seeds.emplace_back(db->coords[i].begin(), db->coords[i].end());
                ++n;
            }
            con.info(std::format("fingerprint: {} nearest neighbours seed S1", n));
        }
    }

    // The checkpoint of an earlier session (spec 04 section 10).
    std::optional<opt::PipelineState> prev;
    const fs::path ckpt = dir / "checkpoint.json";
    if (sp.resume) {
        if (!fs::exists(ckpt)) return fail("nothing to resume: " + ckpt.string() + " does not exist");
        auto st = opt::PipelineState::load(ckpt);
        if (!st) return fail(st.error());
        if (st->dimension != space.size())
            return fail(std::format("the checkpoint is for {} parameters, the plugin has {}", st->dimension, space.size()));
        prev = std::move(*st);
        con.info(std::format("resuming at {:.0f} s and {} evaluations, best {:.3f} dB ({} session(s) before)", prev->elapsed_s, prev->evals,
                             prev->best_loss, prev->sessions));
    }

    opt::PipelineOptions po;
    po.time_cap_s = 60.0 * a.num("--minutes", sp.default_minutes);
    if (prev) {
        const double left = std::max(0.0, po.time_cap_s - prev->elapsed_s);
        po.time_cap_s = prev->elapsed_s + (sp.more_minutes > 0 ? 60.0 * sp.more_minutes : left);
        if (sp.more_minutes <= 0 && left < 1.0)
            return fail("the run used its time cap; give --more-minutes M to continue it");
        po.resume = &*prev;
    }
    po.patience_s = a.num("--patience", po.patience_s);
    po.max_evals = long(a.num("--evals", 0));
    po.restarts = int(a.num("--restarts", po.restarts));
    po.top_k = int(a.num("--top-k", po.top_k));
    po.seed = std::uint64_t(a.num("--seed", 1));
    po.prior = !a.has("--no-prior");
    po.constraints = &constraints;
    po.progress_interval_s = a.num("--progress-s", con.events() ? 1.0 : 15.0);
    if (a.num("--preview-s", 10.0) > 0) po.progress_interval_s = std::min(po.progress_interval_s, a.num("--preview-s", 10.0));
    po.checkpoint_interval_s = a.num("--checkpoint-s", 30.0);
    out.time_cap_s = po.time_cap_s;
    const std::string table_msg = "the plugin changed its parameter table during the run";
    po.abort = [&p] { return p.pool && !p.pool->table_change().empty(); };
    po.cancel = [&con] {
        if (!Console::get().cancel_requested()) return false;
        static std::atomic<bool> announced{false};
        if (!announced.exchange(true)) con.info("cancel requested: finishing the batch in flight, then saving");
        return true;
    };
    po.checkpoint = [&](const opt::PipelineState& st) {
        if (auto r = st.save(ckpt); !r) Console::get().warn(r.error());
    };
    for (const auto& s : split(a.get("--stages"))) po.stages.push_back(std::atoi(s.c_str()));
    po.log = [&con](const std::string& s) { con.info(s); };
    const long evals_before = prev ? prev->evals : 0;
    const double seconds_before = prev ? prev->elapsed_s : 0;
    const auto t0 = std::chrono::steady_clock::now();
    auto add_preset = [&](json::Value& e) {
        if (sp.preset_index > 0) e.set("preset", sp.preset_index).set("presets", sp.preset_total).set("preset_name", sp.preset_name);
    };
    // A/B of the best so far (spec 09 section 3): preview-ab.wav = target, pause, current match, rewritten when the
    // best has changed and --preview-s (10) has passed; 16-bit so a front end can play it from memory.
    const double preview_every = a.num("--preview-s", 10.0);
    auto last_preview = std::chrono::steady_clock::now() - std::chrono::hours(1);
    std::vector<double> previewed;
    auto preview = [&](const opt::Progress& pr) {
        if (preview_every <= 0 || !pr.best_coords || *pr.best_coords == previewed) return;
        if (std::chrono::steady_clock::now() - last_preview < std::chrono::duration<double>(preview_every)) return;
        auto audio = obj.render(*pr.best_coords);
        if (!audio) return;
        last_preview = std::chrono::steady_clock::now();
        previewed = *pr.best_coords;
        const float g = float(std::pow(10.0, pr.best_gain_db / 20.0));
        WavData ab{job.sample_rate, 1, target.mono()};
        ab.samples.resize(ab.samples.size() + std::size_t(0.3 * job.sample_rate), 0.0f);
        for (float s : opt::mono_of(*audio)) ab.samples.push_back(s * g);
        const auto tmp = dir / "preview-ab.tmp", dst = dir / "preview-ab.wav";
        if (!write_wav(tmp, ab, WavFormat::Pcm16)) return;
        std::error_code rec;
        fs::rename(tmp, dst, rec);
        if (rec) return;
        if (con.events()) {
            json::Value e;
            e.set("event", "preview").set("ab", dst.string()).set("best_db", pr.best);
            add_preset(e);
            con.emit(std::move(e));
        }
    };
    po.progress = [&](const opt::Progress& pr) {
        if (sp.on_progress) sp.on_progress(pr);
        preview(pr);
        if (!con.events()) {
            con.info(std::format("  [{:6.0f} s] {:<24} {:7} evals, best {:.3f} dB", pr.seconds, pr.stage, pr.evals, pr.best));
            return;
        }
        const double session_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        double frac = pr.seconds / std::max(1.0, po.time_cap_s);
        if (po.max_evals > 0) frac = std::max(frac, double(pr.evals) / double(po.max_evals));
        json::Value e;
        e.set("event", "progress").set("stage", pr.stage).set("evals", static_cast<std::int64_t>(pr.evals)).set("best_db", pr.best)
            .set("seconds", pr.seconds).set("time_cap_s", po.time_cap_s).set("fraction", std::min(frac, 0.999))
            .set("evals_per_s", session_s > 0 ? double(pr.evals - evals_before) / session_s : 0.0);
        add_preset(e);
        con.emit(std::move(e));
    };
    po.on_stage = [&](const opt::StageReport& r, bool finished) {
        if (!con.events()) return;
        json::Value e;
        e.set("event", "stage").set("name", r.name).set("free", r.free).set("finished", finished).set("before_db", r.before)
            .set("after_db", r.after).set("evals", static_cast<std::int64_t>(r.evals)).set("seconds", r.seconds);
        add_preset(e);
        con.emit(std::move(e));
    };
    (void)seconds_before;
    con.info(std::format("fit: {} {} with {} active and {} categorical parameters, {} worker(s), mode {}", host::to_string(p.info.format),
                         p.info.name, space.active().size(), space.categorical().size(), p.workers(), to_string(so.mode)));
    if (con.events()) {
        json::Value e;
        e.set("event", "phase").set("name", "fitting").set("active", space.active().size()).set("categorical", space.categorical().size())
            .set("time_cap_s", po.time_cap_s).set("workers", p.workers());
        add_preset(e);
        con.emit(std::move(e));
    }

    out.result = opt::run_pipeline(obj, space, po, seeds);
    const auto& res = out.result;
    out.seconds = res.seconds;
    out.evals = res.evals;
    // Learned and updated constraints go back into the profile (spec 05 section 4), even when the fit failed.
    if (!constraints.empty() || (stored && !stored->constraints.empty()) || pitch_measured) {
        opt::Profile prof = stored ? *stored : opt::Profile{};
        if (!stored) {
            prof.format = p.info.format;
            prof.uid = p.info.uid;
            prof.name = p.info.name;
            prof.vendor = p.info.vendor;
            prof.version_hash = p.info.version_hash;
        }
        prof.constraints = constraints.constraints();
        if (pitch_measured) prof.pitch = pitch;
        const auto path = opt::Profile::default_path(profiles_dir(a), p.info);
        if (auto r = prof.save(path); !r) con.warn(r.error());
    }
    if (res.aborted) return fail(table_msg + " (" + (p.pool ? p.pool->table_change() : std::string()) + "); ids would no longer mean the same parameters (spec 05 section 9)");
    if (!res.best.eval.ok() || !std::isfinite(res.best.eval.loss)) {
        if (res.cancelled) {
            out.status = "cancelled";  // nothing valid yet, but the checkpoint continues the run
            return out;
        }
        return fail("no valid candidate was found");
    }
    out.have_best = true;
    out.loss_db = res.best.eval.loss;
    out.gain_db = res.best.eval.score.gain_db;
    out.status = res.cancelled ? "cancelled" : "finished";

    // Results (spec 07 section 1), written for a cancelled run too: its best so far is a usable preset.
    if (con.events()) con.emit(json::Value().set("event", "phase").set("name", "writing results"));
    std::error_code ec;
    fs::create_directories(dir / "top-k", ec);
    if (ec) return fail("cannot create " + dir.string());
    auto text = [&](std::uint32_t id, double u) { return p.value_text(id, u); };
    auto save_match = [&](const opt::Candidate& c, const fs::path& stem) {
        auto pj = output::params_json(p.info, p.params, c.coords, text);
        pj.set("loss_db", c.eval.loss);
        pj.set("gain_db", c.eval.score.gain_db);
        json::Object losses;
        for (std::size_t f = 0; f < features::kFeatureCount; ++f)
            losses[std::string(features::to_string(features::Feature(f)))] = c.eval.score.loss[f];
        pj.set("features", json::Value(std::move(losses)));
        pj.set("notes", host::to_json(job)["notes"]);
        if (auto w = output::write_text(fs::path(stem.string() + ".params.json"), pj.dump(2) + "\n"); !w) con.warn(w.error());
        if (auto audio = obj.render(c.coords)) {
            WavData w{audio->sample_rate, audio->channels, audio->samples};
            (void)write_wav(fs::path(stem.string() + ".wav"), w);
        }
    };
    save_match(res.best, dir / "best");
    fs::rename(dir / "best.wav", dir / "match.wav", ec);
    for (std::size_t i = 0; i < res.top_k.size(); ++i) save_match(res.top_k[i], dir / "top-k" / std::to_string(i + 1));

    // crashes/: one report per distinct failure (spec 05 section 7), replayable with `mimicry replay`.
    int reports = 0;
    for (const auto& f : res.failures) {
        output::FailureReport fr;
        fr.info = p.info;
        fr.plugin_path = fs::absolute(p.path).string();
        fr.job = job;
        fr.job.coords = f.coords;
        fr.outcome = f.outcome;
        fr.set_params(p.params);
        for (std::size_t i = 0; i < fr.params.size(); ++i) fr.params[i].text = text(p.params[i].id, f.coords[i]);
        for (std::size_t k = 0; k < f.minimal.size(); ++k)
            fr.minimal.push_back({p.params[f.minimal[k]].id, p.params[f.minimal[k]].name, k < f.threshold.size() ? f.threshold[k] : 0.0});
        if (auto w = fr.write(dir / "crashes"); w) ++reports;
        else con.warn(w.error());
    }

    // ab.wav: target, 0.3 s of silence, match scaled by the solved gain (same scale for both) [S]; diff.ppm.
    if (auto audio = obj.render(res.best.coords)) {
        const auto mono = opt::mono_of(*audio);
        const float g = float(std::pow(10.0, res.best.eval.score.gain_db / 20.0));
        WavData ab{job.sample_rate, 1, target.mono()};
        ab.samples.resize(ab.samples.size() + std::size_t(0.3 * job.sample_rate), 0.0f);
        for (float s : mono) ab.samples.push_back(s * g);
        (void)write_wav(dir / "ab.wav", ab, WavFormat::Pcm16);
        const auto img = output::spectrogram_comparison(target.mono(), mono, job.sample_rate, res.best.eval.score.gain_db);
        if (auto w = output::write_bytes(dir / "diff.ppm", output::ppm_bytes(img)); !w) con.warn(w.error());
        (void)output::write_bytes(dir / "diff.bmp", output::bmp_bytes(img));
    }

    // best.state: the plugin's own state after rendering the match; best.vstpreset for a VST3 target (T14).
    Written written;
    {
        host::Job j = job;
        j.coords = res.best.coords;
        std::optional<std::vector<std::uint8_t>> state;
        if (p.pool) {
            if (auto c = supervisor::WorkerClient::open(p.path, p.info.uid, worker_options(a)); c) {
                if ((*c)->render(j)) {
                    if (auto st = (*c)->save_state()) state = std::move(*st);
                }
            }
        } else if (p.local) {
            std::lock_guard lock(p.local_mutex);
            if (p.local->render(j)) {
                if (auto st = p.local->save_state()) state = std::move(*st);
            }
        }
        if (state) {
            written.state = bool(output::write_bytes(dir / "best.state", *state));
            if (p.info.format == host::PluginFormat::Vst3) {
                if (auto vp = output::vstpreset_from_state(p.info.uid, *state)) written.vstpreset = bool(output::write_bytes(dir / "best.vstpreset", *vp));
                else con.warn("best.vstpreset: " + vp.error());
            }
        } else {
            con.warn("best.state could not be saved");
        }
    }

    // Parameters at a bound of their searched range: the plugin probably cannot reach the target there.
    std::vector<std::string> at_bound;
    for (auto i : space.active()) {
        const auto& s = space.slot(i);
        const double span = s.hi - s.lo;
        if (span <= 1e-9) continue;
        const double x = res.best.coords[i];
        if (x - s.lo < 0.01 * span || s.hi - x < 0.01 * span)
            at_bound.push_back(std::format("{} = {} ({} bound)", p.params[i].name, text(p.params[i].id, x), x - s.lo < 0.01 * span ? "lower" : "upper"));
    }

    json::Value result;
    result.set("status", out.status);
    result.set("format", std::string(host::to_string(p.info.format)));
    result.set("plugin", p.info.name);
    result.set("uid", p.info.uid);
    result.set("version_hash", p.info.version_hash);
    result.set("target", sp.target_label);
    result.set("kind", std::string(features::to_string(an.kind)));
    result.set("mode", std::string(to_string(so.mode)));
    result.set("loss_db", res.best.eval.loss);
    result.set("gain_db", res.best.eval.score.gain_db);
    result.set("evals", static_cast<std::int64_t>(res.evals));
    result.set("seconds", res.seconds);
    result.set("sessions", res.state.sessions);
    result.set("cancelled", res.cancelled);
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
    result.set("failure_reports", static_cast<std::int64_t>(res.failures.size()));
    json::Array bound;
    for (const auto& b : at_bound) bound.push_back(b);
    result.set("at_bound", json::Value(std::move(bound)));
    double residual = 0;
    if (pitched) {
        if (auto audio = obj.render(res.best.coords)) residual = opt::pitch_residual_cents(*audio, an.f0);
        json::Value pj;
        pj.set("method", std::string(to_string(pitch.method)));
        pj.set("bend_range", pitch.bend_range);
        pj.set("target_f0", an.f0);
        pj.set("key", an.key);
        pj.set("cents", an.cents);
        pj.set("residual_cents", residual);  // measured pitch of the match against the target (spec 02 section 5)
        result.set("pitch", std::move(pj));
    }
    json::Array topk;
    for (const auto& t : res.top_k) topk.push_back(t.eval.loss);
    result.set("top_k_loss_db", json::Value(std::move(topk)));
    json::Value files;
    files.set("params", "best.params.json").set("match", "match.wav").set("ab", "ab.wav").set("report", "report.md")
        .set("spectrogram", "diff.bmp");
    if (written.state) files.set("state", "best.state");
    if (written.vstpreset) files.set("vstpreset", "best.vstpreset");
    result.set("files", std::move(files));
    if (auto w = output::write_text(dir / "result.json", result.dump(2) + "\n"); !w) return fail(w.error());
    if (auto w = output::write_text(dir / "report.md", report_markdown(sp, out, space, p.params, p.info, an, out.status, out.floor_db, at_bound,
                                                                      res.state.sessions, constraints, pitch_line));
        !w)
        con.warn(w.error());

    std::string summary = std::format("\n{} {:.3f} dB (gain {:+.1f} dB) after {} evaluations in {:.0f} s\n", res.cancelled ? "cancelled, best" : "best",
                                      res.best.eval.loss, res.best.eval.score.gain_db, res.evals, res.seconds);
    for (const auto& s : res.stages)
        summary += std::format("  {:<26} {:3} free  {:8.3f} -> {:8.3f} dB  {:6} evals\n", s.name, s.free, s.before, s.after, s.evals);
    for (const auto& [o, n] : res.outcomes)
        if (o != Outcome::Ok) summary += std::format("  {} {} candidate(s)\n", n, to_string(o));
    for (const auto& c : res.learned) summary += "  learned: " + c.describe() + "\n";
    if (pitched) summary += std::format("  pitch: {}, residual {:+.1f} cents\n", to_string(pitch.method), residual);
    if (res.resampled + res.skipped > 0)
        summary += std::format("  {} sample(s) redrawn and {} seed point(s) skipped inside known constraints\n", res.resampled, res.skipped);
    if (reports) summary += std::format("  {} failure report(s) in crashes/ (mimicry replay <file>)\n", reports);
    if (!at_bound.empty()) summary += std::format("  {} parameter(s) at a bound of their range (see report.md)\n", at_bound.size());
    summary += "wrote " + dir.string() + " (listen to ab.wav)";
    con.info(summary);
    return out;
}

}  // namespace mimicry::app
