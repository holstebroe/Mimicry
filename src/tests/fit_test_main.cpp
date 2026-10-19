// Optimizer against a real plugin (WP3): the probe classifies mimicry_test_synth's parameters and finds its
// safe box, the profile round-trips, the pipeline recovers a preset of the plugin's own (a small T6) and the
// fingerprint database seeds from the right neighbourhood; failure bisection finds the planted crash (T8),
// and its failure report replays with `mimicry replay` (spec 05 section 7). Renders go through a worker pool, as in
// production (CLAUDE.md rule 4). Usage: mimicry_fit_test <plugin> <mimicry_worker> [<mimicry>].

#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <format>
#include <string>
#include <utility>

#if !defined(_WIN32)
#include <sys/wait.h>
#endif

#include "features/Loss.hpp"
#include "features/Pitch.hpp"
#include "host/ParamUtil.hpp"
#include "opt/Constraints.hpp"
#include "output/FailureReport.hpp"
#include "opt/Fingerprint.hpp"
#include "opt/Objective.hpp"
#include "opt/Pipeline.hpp"
#include "opt/PitchCalibration.hpp"
#include "opt/Probe.hpp"
#include "opt/Space.hpp"
#include "supervisor/WorkerPool.hpp"
#include "tests/test_synth/TestSynthCore.hpp"

namespace {

using namespace mimicry;
using namespace mimicry::opt;
namespace ts = mimicry::testsynth;

int failures = 0;

void check(bool ok, const std::string& what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
        ++failures;
    }
}

const ParamProfile* entry(const Profile& p, ts::ParamId id) {
    for (const auto& e : p.params)
        if (e.id == id) return &e;
    return nullptr;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fputs("usage: mimicry_fit_test <plugin> <mimicry_worker>\n", stderr);
        return 2;
    }
    supervisor::WorkerOptions wo;
    wo.worker_exe = argv[2];
    wo.min_timeout_s = 1.0;
    wo.timeout_factor = 0;
    wo.shm_bytes = 16u << 20;
    auto pool = supervisor::WorkerPool::open(argv[1], {}, 3, wo);
    check(pool.has_value(), "open worker pool");
    if (!pool) return 1;
    auto& p0 = (*pool)->client(0);
    const auto info = p0.info();
    const auto params = p0.parameters();
    RenderBatch render = [&](const std::vector<host::Job>& jobs) { return (*pool)->render_all(jobs); };

    host::Job job;
    job.notes.push_back({57, 0, 0.8f, 0, 400});
    job.tail_max_ms = 1200;

    // --- Probe (spec 04 section 3) ---
    ProbeOptions po;
    po.job = job;
    po.pair_params = 6;
    auto prof = probe(render, info, params, po);
    check(prof.has_value(), "probe: " + (prof ? std::string() : prof.error()));
    if (!prof) return 1;
    auto cls = [&](ts::ParamId id) { return entry(*prof, id) ? entry(*prof, id)->measured : ProbeClass::Spectrum; };
    std::string table;
    for (const auto& e : prof->params)
        table += std::format("  {:<14} {:<10} effect {:7.2f} {} safe [{:.2f}, {:.2f}]\n", e.name, to_string(e.measured),
                             e.effect, e.response == Response::Log ? "log" : e.response == Response::Exp ? "exp" : "lin",
                             e.safe_lo, e.safe_hi);
    std::printf("%s probe (floor %.3f dB, %.2f ms/render):\n%s", std::string(host::to_string(info.format)).c_str(),
                prof->floor_db, prof->render_ms, table.c_str());
    // At the defaults the oscillator is a sine, so the cutoff changes the level, not the brightness: the probe
    // reports what a parameter does at the defaults (spec 04 section 3).
    check(cls(ts::kCutoff) == ProbeClass::Level || cls(ts::kCutoff) == ProbeClass::Brightness, "probe: cutoff is level or brightness");
    check(cls(ts::kWave) != ProbeClass::Dead && cls(ts::kWave) != ProbeClass::Pitch, "probe: wave changes the spectrum, not the pitch");
    check(cls(ts::kDetune) == ProbeClass::Pitch, "probe: detune is pitch");
    check(cls(ts::kLevel) == ProbeClass::Level, "probe: level is level");
    check(cls(ts::kRelease) == ProbeClass::Time || cls(ts::kDecay) == ProbeClass::Time, "probe: an envelope time is time");
    check(cls(ts::kLfoRate) == ProbeClass::Dead, "probe: LFO rate is dead at LFO depth 0");
    check(cls(ts::kHazA) == ProbeClass::Dead && cls(ts::kHazB) == ProbeClass::Dead, "probe: single hazard switches are dead");
    check(!entry(*prof, ts::kHidden) && !entry(*prof, ts::kMeter) && !entry(*prof, ts::kProgram) && !entry(*prof, ts::kBypass),
          "probe: hidden, read-only, program and bypass are not probed");
    const auto* nan = entry(*prof, ts::kHazNan);
    check(nan && nan->outcomes.back() == Outcome::NonFinite && nan->safe_hi == 0.75 && nan->safe_lo == 0,
          "probe: the NaN hazard shortens the safe box to the last good value");
    const auto* hang = entry(*prof, ts::kHazHang);
    check(hang && hang->outcomes.back() == Outcome::Timeout && hang->safe_hi == 0.75, "probe: the hang hazard is a timeout");
    check(!prof->pairs.empty(), "probe: pair interactions measured");
    check(prof->pitch.method == PitchMethod::Tuning && std::abs(prof->pitch.tuning_cents - 50) < 3,
          std::format("probe: the plugin takes tuning ({} {:.1f} cents)", to_string(prof->pitch.method), prof->pitch.tuning_cents));
    {
        // Spec 02 section 5: with tuning ignored, a full MIDI pitch bend measures the bend range (set to 7).
        host::Job cj = job;
        for (const auto& q : params) cj.coords.push_back(q.default_coord);
        cj.coords[ts::kIgnoreTuning] = 1.0;
        cj.coords[ts::kBendRange] = info.format == host::PluginFormat::Clap ? host::plain_to_coord(params[ts::kBendRange], 7.0)
                                                                            : (7.0 - 1.0) / 23.0;
        const auto bc = calibrate_pitch(render, cj);
        check(bc.method == PitchMethod::Bend && std::abs(bc.bend_range - 7.0) < 0.1,
              std::format("pitch: bend measured when tuning is ignored ({} range {:.3f}, tuning {:.1f} cents)",
                          to_string(bc.method), bc.bend_range, bc.tuning_cents));
        // The bend plays the cents: 30 cents through a 7-semitone bend lands within 3 cents.
        auto played = cj;
        played.notes[0].cents = 30;
        apply_pitch_method(bc, played);
        const auto r = render({cj, played});
        if (r.size() == 2 && r[0] && r[1]) {
            const double f0 = features::estimate_f0(mono_of(*r[0]), job.sample_rate).f0;
            const double res = pitch_residual_cents(*r[1], f0 * std::pow(2.0, 30.0 / 1200.0));
            check(std::abs(res) < 3, std::format("pitch: 30 cents through the bend, residual {:+.2f} cents", res));
        } else {
            check(false, "pitch: bend renders");
        }
    }
    for (auto id : {ts::kHazNan, ts::kHazHang, ts::kHazPoison, ts::kHazSticky, ts::kHazThrow}) {
        const auto* e = entry(*prof, id);
        check(e && e->dead() && e->outcomes.back() != Outcome::Ok && e->safe_hi == 0.75,
              "probe: a hazard switch that only fails is dead, its failure kept: " + (e ? e->name : std::to_string(id)));
    }

    // --- T8: bisection finds the planted crash {Hazard A > 0.8, Hazard B > 0.9} within 2 % (spec 05 section 4) ---
    {
        std::vector<double> good;
        for (const auto& q : params) good.push_back(q.default_coord);
        auto bad = good;
        bad[ts::kHazA] = 0.95;
        bad[ts::kHazB] = 0.97;
        bad[ts::kWave] = 1.0;  // harmless changes ddmin must drop
        bad[ts::kCutoff] = 0.3;
        bad[ts::kSustain] = 0.4;
        bad[ts::kDecay] = 0.6;
        OutcomeBatch test = [&](const std::vector<std::vector<double>>& vs) {
            std::vector<host::Job> jobs;
            for (const auto& v : vs) {
                host::Job j = job;
                j.coords = v;
                jobs.push_back(std::move(j));
            }
            std::vector<Outcome> out;
            for (const auto& r : render(jobs)) out.push_back(r ? Outcome::Ok : r.error());
            return out;
        };
        const auto r = bisect_failure(test, good, bad, Outcome::Crash);
        std::string found;
        for (std::size_t k = 0; k < r.minimal.size(); ++k)
            found += std::format(" {}={:.4f}", params[r.minimal[k]].name, k < r.threshold.size() ? r.threshold[k] : -1.0);
        std::printf("T8: reproduced %d, minimal set:%s (%ld renders)\n", int(r.reproduced), found.c_str(), r.renders);
        const bool pair = r.minimal.size() == 2 && r.minimal[0] == ts::kHazA && r.minimal[1] == ts::kHazB;
        check(r.reproduced && pair, "T8: minimal failing set is {Hazard A, Hazard B}");
        check(pair && std::abs(r.threshold[0] - 0.8) < 0.02 && std::abs(r.threshold[1] - 0.9) < 0.02, "T8: thresholds within 2 %");
        if (pair) {
            ConstraintSet set(params);
            set.add(make_constraint(r, bad, Outcome::Crash, params));
            auto in = good, out = good;
            in[ts::kHazA] = 0.85, in[ts::kHazB] = 0.93;
            out[ts::kHazA] = 0.85, out[ts::kHazB] = 0.85;
            check(set.find(in) && !set.find(out), "T8: the stored region rejects the crash corner only");
        }

        // Spec 05 section 7: the failure report round-trips and replays.
        const auto rdir = std::filesystem::temp_directory_path() / ("mimicry_crashes_" + std::string(host::to_string(info.format)));
        std::filesystem::remove_all(rdir);
        output::FailureReport fr;
        fr.info = info;
        fr.plugin_path = std::filesystem::absolute(argv[1]).string();
        fr.job = job;
        fr.job.coords = bad;
        fr.outcome = Outcome::Crash;
        fr.set_params(params);
        for (std::size_t k = 0; k < r.minimal.size(); ++k)
            fr.minimal.push_back({params[r.minimal[k]].id, params[r.minimal[k]].name, r.threshold[k]});
        auto nan_report = fr;
        nan_report.job.coords = good;
        nan_report.job.coords[ts::kHazNan] = 1.0;
        nan_report.outcome = Outcome::NonFinite;
        nan_report.minimal.clear();
        nan_report.set_params(params);
        const auto crash_path = fr.write(rdir);
        const auto nan_path = nan_report.write(rdir);
        check(crash_path && nan_path && *crash_path != *nan_path, "failure reports written, distinct files");
        if (crash_path && nan_path) {
            auto back = output::FailureReport::load(*crash_path);
            check(back && back->outcome == Outcome::Crash && back->minimal.size() == r.minimal.size() &&
                      back->coords_for(params) == bad && back->job.notes.size() == job.notes.size() && back->hash() == fr.hash(),
                  "failure report round trip");
            auto shuffled = params;  // matched on id + name, not position (rule 7)
            std::swap(shuffled[ts::kHazA], shuffled[ts::kHazB]);
            const auto moved = back ? back->coords_for(shuffled) : std::vector<double>{};
            check(moved.size() == params.size() && moved[ts::kHazA] == bad[ts::kHazB] && moved[ts::kHazB] == bad[ts::kHazA],
                  "failure report values follow ids and names");
            if (argc > 3) {
                auto run = [&](const std::string& extra, const std::filesystem::path& file) {
#if defined(_WIN32)
                    // cmd.exe strips the outer quotes of a line that starts with one.
                    const std::string cmd = std::format("\"\"{}\" replay \"{}\" {} > NUL 2>&1\"", argv[3], file.string(), extra);
                    return std::system(cmd.c_str());
#else
                    const std::string cmd = std::format("\"{}\" replay \"{}\" {} > /dev/null 2>&1", argv[3], file.string(), extra);
                    const int st = std::system(cmd.c_str());
                    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
#endif
                };
                check(run("--isolated", *crash_path) == 0, "replay --isolated reproduces the crash");
                check(run("", *nan_path) == 0, "replay in-process reproduces the NaN");
                check(run("--isolated", *nan_path) == 0, "replay --isolated reproduces the NaN");
            }
        }
        std::filesystem::remove_all(rdir);
    }

    const auto dir = std::filesystem::temp_directory_path() / ("mimicry_fit_test_" + std::string(host::to_string(info.format)));
    std::filesystem::create_directories(dir);
    const auto ppath = Profile::default_path(dir, info);
    check(prof->save(ppath).has_value(), "profile save");
    auto loaded = Profile::load(ppath);
    check(loaded && loaded->matches(info) && loaded->params.size() == prof->params.size(), "profile reload");

    // --- Fit a preset of the plugin's own (a small T6) ---
    SpaceOptions spo;
    spo.pin_pitch = true;
    auto space = Space::build(params, info.format, &*loaded, spo);
    std::printf("space: %zu active, %zu categorical\n", space.active().size(), space.categorical().size());
    check(space.slot(ts::kHazNan).hi <= 0.75 && space.slot(ts::kLfoRate).role == Role::Fixed &&
              space.slot(ts::kLfoSeed).role == Role::Fixed && space.slot(ts::kWave).role == Role::Categorical &&
              space.slot(ts::kDetune).role == Role::Fixed,
          "space from the profile: safe box, dead, seed and pitch fixed, wave categorical");
    // The target preset in plain units, so it is the same sound in both formats: CLAP coordinates decode
    // plain values (log for Hz/ms), the VST3 build (clap-wrapper) normalises them linearly.
    auto coord_of = [&](ts::ParamId id, double plain) {
        if (info.format == host::PluginFormat::Clap) return host::plain_to_coord(params[id], plain);
        return (plain - ts::kParams[id].min) / (ts::kParams[id].max - ts::kParams[id].min);
    };
    auto truth = space.defaults();
    truth[ts::kWave] = 1.0;
    truth[ts::kCutoff] = coord_of(ts::kCutoff, 1500);
    truth[ts::kDecay] = coord_of(ts::kDecay, 300);
    truth[ts::kSustain] = coord_of(ts::kSustain, 30);
    truth[ts::kDetune] = coord_of(ts::kDetune, 16);
    std::printf("truth: cutoff %s, decay %s, sustain %s, detune %s\n", p0.value_text(ts::kCutoff, truth[ts::kCutoff]).c_str(),
                p0.value_text(ts::kDecay, truth[ts::kDecay]).c_str(), p0.value_text(ts::kSustain, truth[ts::kSustain]).c_str(),
                p0.value_text(ts::kDetune, truth[ts::kDetune]).c_str());
    host::Job tj = job;
    tj.coords = truth;
    auto tr = render({tj});
    check(!tr.empty() && tr[0].has_value(), "render the target preset: " + (tr.empty() || tr[0] ? std::string() : std::string(to_string(tr[0].error())) + " " + p0.last_message()));
    if (tr.empty() || !tr[0]) return 1;
    features::AnalyzeOptions ao;
    ao.sample_rate = job.sample_rate;
    auto target = features::Target::from_mono(mono_of(*tr[0]), job.sample_rate, ao);
    check(target.has_value(), "target analysis");
    if (!target) return 1;
    // S0: the measured pitch is played (key + cents); the detune parameter stays at its default.
    const auto& an = target->analysis();
    std::printf("target: f0 %.2f Hz, key %d %+.1f cents, kind %s\n", an.f0, an.key, an.cents,
                std::string(features::to_string(an.kind)).c_str());
    check(an.key == 57 && std::abs(an.cents - 16.0) < 3.0, "S0 measures the target's pitch (key 57, +16 cents)");
    job.notes[0].key = an.key;
    job.notes[0].cents = an.cents;
    features::ScorerOptions so;
    so.solve_gain = space.level_index() < 0;  // spec 03 section 4: the level parameter matches the level
    check(space.level_index() == long(ts::kLevel), "the level parameter is found");
    AudioObjective obj(render, job, *target, features::Weights::preset(target->analysis().kind), so, 3);
    check(obj.calibrate_offset(space.defaults()), "onset offset from the defaults");

    PipelineOptions opt;
    opt.max_evals = 4000;
    opt.time_cap_s = 240;
    opt.seed_samples = 150;
    opt.restarts = std::getenv("FIT_R") ? std::atoi(std::getenv("FIT_R")) : 2;
    opt.seed = std::getenv("FIT_SEED") ? std::uint64_t(std::atoll(std::getenv("FIT_SEED"))) : 3;
    opt.log = [](const std::string& s) { std::printf("  %s\n", s.c_str()); };
    const auto res = run_pipeline(obj, space, opt);
    std::printf("fit: loss %.3f dB after %ld evals in %.1f s (cache hits %zu); cutoff %.3f (truth %.3f), sustain %.3f "
                "(truth %.3f), wave %.0f\n", res.best.eval.loss, res.evals, res.seconds, obj.cache().hits(),
                res.best.coords[ts::kCutoff], truth[ts::kCutoff], res.best.coords[ts::kSustain], truth[ts::kSustain],
                res.best.coords[ts::kWave]);
    for (auto i : space.active())
        std::printf("  %-12s found %.3f (%s)  truth %.3f (%s)\n", params[i].name.c_str(), res.best.coords[i],
                    p0.value_text(params[i].id, res.best.coords[i]).c_str(), truth[i], p0.value_text(params[i].id, truth[i]).c_str());
    for (const auto& st : res.stages)
        std::printf("  stage %-22s %2d free %8.3f -> %8.3f dB %6ld evals %5.1f s\n", st.name.c_str(), st.free, st.before, st.after,
                    st.evals, st.seconds);
    auto played = truth;  // the true preset with its pitch moved from the detune parameter to the note
    played[ts::kDetune] = space.defaults()[ts::kDetune];
    const auto self = obj.evaluate({played}, false);
    std::printf("  loss of the true preset played at the measured pitch: %.3f dB\n", self[0].loss);
    check(res.best.eval.ok() && res.best.eval.loss < 1.0, "fit recovers the preset (loss under 1 dB)");
    if (auto best = obj.render(res.best.coords)) {
        const double residual = pitch_residual_cents(*best, an.f0);
        std::printf("  pitch residual of the match: %+.2f cents\n", residual);
        check(std::abs(residual) < 3, "fit: the match's pitch is within 3 cents of the target");
    }
    check(res.best.coords[ts::kWave] == 1.0, "fit: wave (categorical) recovered");
    const double cutoff_hz = std::atof(p0.value_text(ts::kCutoff, res.best.coords[ts::kCutoff]).c_str());
    std::printf("  cutoff found: %.0f Hz (truth 1500 Hz)\n", cutoff_hz);
    check(std::abs(std::log2(cutoff_hz / 1500.0)) < 0.25, "fit: cutoff recovered within a quarter octave");

    for (std::size_t i = 0; i < res.best.coords.size(); ++i)
        if (space.slot(i).role == Role::Fixed)
            check(res.best.coords[i] == space.defaults()[i], "fixed parameter unchanged: " + params[i].name);
    check(!res.top_k.empty(), "top-K answers");

    // --- Fingerprints (spec 04 section 7) ---
    FingerprintOptions fo;
    fo.job = job;
    fo.count = 240;
    fo.seed = 9;
    fo.threads = 3;
    auto db = build_fingerprints(render, info, space, fo);
    check(db.size() > 200 && db.matches(info, params), "fingerprint database built");
    const auto fpath = FingerprintDb::default_path(dir, info);
    check(db.save(fpath).has_value(), "fingerprint save");
    auto db2 = FingerprintDb::load(fpath);
    check(db2 && db2->size() == db.size(), "fingerprint reload");
    if (db2) {
        Embedder em(job.sample_rate);
        const auto nn = db2->nearest(em.embed(target->mono()), 5);
        double near = 0, rnd = 0;
        for (auto i : nn) near += std::abs(db2->coords[i][ts::kCutoff] - truth[ts::kCutoff]);
        for (std::size_t i = 0; i < 5; ++i) rnd += std::abs(db2->coords[i][ts::kCutoff] - truth[ts::kCutoff]);
        std::printf("fingerprint: mean cutoff error of 5 nearest %.3f, of 5 arbitrary %.3f\n", near / 5, rnd / 5);
        check(near < rnd, "fingerprint neighbours are closer to the target preset than arbitrary entries");
    }
    std::filesystem::remove_all(dir);
    const auto st = (*pool)->total_stats();
    std::printf("workers: %llu spawned, %llu crashes, %llu timeouts\n", (unsigned long long)st.spawned,
                (unsigned long long)st.crashes, (unsigned long long)st.timeouts);

    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::puts("mimicry_fit_test: all passed");
    return 0;
}
