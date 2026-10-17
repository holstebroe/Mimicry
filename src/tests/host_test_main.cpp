// Host tests against mimicry_test_synth (in-process, for debugging the backend; T1-T3, T9 and parts of
// T13/T16 of docs/spec/07_OUTPUT_VALIDATION.md). Usage: mimicry_host_test <plugin.clap|plugin.vst3>.

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <string>

#include "core/Fft.hpp"
#include "host/PluginHost.hpp"
#include "tests/test_synth/TestSynthCore.hpp"

namespace {

using namespace mimicry;
using namespace mimicry::host;
namespace ts = mimicry::testsynth;

int failures = 0;

void check(bool ok, const std::string& what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
        ++failures;
    }
}

std::size_t index_of(const Instance& inst, std::uint32_t id) {
    const auto& ps = inst.parameters();
    for (std::size_t i = 0; i < ps.size(); ++i)
        if (ps[i].id == id) return i;
    return 0;
}

// Coordinates at the plugin's defaults.
std::vector<double> defaults(const Instance& inst) {
    std::vector<double> c;
    for (const auto& p : inst.parameters()) c.push_back(p.default_coord);
    return c;
}

// Sets a parameter by its plain value.
void set_plain(const Instance& inst, std::vector<double>& coords, ts::ParamId id, double plain) {
    // The synth's own table gives the plain range; both formats' coordinates are linear or log in it
    // (the VST3 wrapper maps normalised linearly to the CLAP plain range, so the CLAP decode applies).
    const auto i = index_of(inst, id);
    const auto& want = ts::kParams[id];
    coords[i] = (plain - want.min) / (want.max - want.min);
    if (inst.parameters()[i].log_scale) coords[i] = std::log(plain / want.min) / std::log(want.max / want.min);
}

Job make_job(const Instance& inst, double on_ms = 0, double off_ms = 500, int key = 60, double cents = 0) {
    Job job;
    job.coords = defaults(inst);
    set_plain(inst, job.coords, ts::kLfoSeed, 1);  // deterministic
    set_plain(inst, job.coords, ts::kLfoDepth, 0);
    job.notes.push_back({key, cents, 0.8f, on_ms, off_ms});
    return job;
}

double dominant_hz(const Audio& a, double from_s, double to_s) {
    const std::size_t n = 32768;
    std::vector<float> seg(n, 0.0f);
    const auto start = static_cast<std::size_t>(from_s * a.sample_rate);
    for (std::size_t i = 0; i < n && (start + i) * 2 < a.samples.size() && start + i < static_cast<std::size_t>(to_s * a.sample_rate); ++i)
        seg[i] = a.samples[(start + i) * 2];
    Fft fft(n);
    const auto mag = fft.magnitude(seg.data());
    const auto peak = std::max_element(mag.begin() + 1, mag.end()) - mag.begin();
    return static_cast<double>(peak) * a.sample_rate / static_cast<double>(n);
}

void test_enumeration(Instance& inst) {
    const auto& info = inst.info();
    const bool clap = info.format == PluginFormat::Clap;
    check(!info.uid.empty() && (!clap || info.uid == "org.mimicry.test-synth"), "uid: " + info.uid);
    check(info.kind == PluginKind::Instrument, "kind is Instrument");
    check(!info.version_hash.empty() && info.version_hash != "unknown", "version hash is set");

    const auto& ps = inst.parameters();
    check(ps.size() == ts::kParamCount, "parameter count " + std::to_string(ps.size()));
    if (ps.size() != ts::kParamCount) return;
    for (std::uint32_t i = 0; i < ts::kParamCount; ++i) {
        const auto& want = ts::kParams[i];
        const auto& got = ps[i];
        check(got.id == want.id && got.name == want.name && got.module == want.module,
              std::string("id/name/module of ") + want.name);
        check(got.units == want.unit, std::string("units of ") + want.name + ": '" + got.units + "'");
        check(got.flags.hidden == want.hidden && got.flags.readonly == want.readonly && got.flags.bypass == want.bypass,
              std::string("flags of ") + want.name);
        check((got.steps > 0) == want.stepped, std::string("stepped flag of ") + want.name);
        // CLAP exposes plain ranges and a log/linear decode; VST3 the plugin's own normalised coordinate.
        if (clap) {
            check(got.log_scale == (want.scale == ts::Scale::Log && !want.stepped),
                  std::string("log decision of ") + want.name);
            check(std::fabs(got.min - want.min) < 1e-9 && std::fabs(got.max - want.max) < 1e-9,
                  std::string("range of ") + want.name);
        } else {
            check(!got.log_scale && got.min == 0 && got.max == 1, std::string("normalised range of ") + want.name);
        }
        check(!got.texts[0].empty() && !got.texts[4].empty(), std::string("value texts of ") + want.name);
    }
    check(ps[ts::kProgram].flags.program_change, "program parameter is flagged");
    check(!ps[ts::kWave].flags.program_change, "wave is not a program parameter");
    check(inst.value_text(ts::kCutoff, 1.0).find("20000") != std::string::npos, "value_text(cutoff, 1) = " + inst.value_text(ts::kCutoff, 1.0));
    check(inst.value_text(ts::kWave, 0.0) == "Sine" && inst.value_text(ts::kWave, 1.0) == "Saw", "enum value texts");
}

void test_rendering(Instance& inst) {
    auto job = make_job(inst, 100, 600);
    auto a = inst.render(job);
    check(a.has_value(), "render ok: " + (a ? std::string() : std::string(to_string(a.error())) + " " + inst.last_message()));
    if (!a) return;
    check(a->channels == 2 && a->sample_rate == 48000, "audio format");
    // Audio starts at the plan origin (settle excluded): silence for 100 ms, then the note.
    const auto frames = a->samples.size() / 2;
    check(frames >= 28800, "at least the note plan (" + std::to_string(frames) + " frames)");
    float pre = 0, post = 0;
    for (std::size_t i = 0; i < 4700; ++i) pre = std::max(pre, std::fabs(a->samples[i * 2]));
    for (std::size_t i = 5000; i < 9000; ++i) post = std::max(post, std::fabs(a->samples[i * 2]));
    check(pre == 0.0f && post > 0.01f, "note-on is sample-exact (silent before 100 ms)");
    const double f = dominant_hz(*a, 0.15, 0.55);
    check(std::fabs(f - 261.63) < 3.0, "C4 is 261.6 Hz: " + std::to_string(f));
    check(frames < 48000 * 3, "tail detection ends the render early");

    // +100 cents is one semitone up (note expression / tuning).
    auto up = make_job(inst, 0, 500, 60, 100.0);
    auto b = inst.render(up);
    check(b.has_value(), "tuned render ok");
    if (b) {
        const double fb = dominant_hz(*b, 0.1, 0.45);
        check(std::fabs(fb - 277.18) < 3.0, "+100 cents is C#4: " + std::to_string(fb));
    }
}

void test_repeatability(Instance& inst) {
    auto job = make_job(inst);
    auto a = inst.render(job), b = inst.render(job);
    check(a && b && a->samples == b->samples, "deterministic job renders identically twice");
    set_plain(inst, job.coords, ts::kLfoSeed, 0);
    set_plain(inst, job.coords, ts::kLfoDepth, 100);
    auto c = inst.render(job), d = inst.render(job);
    check(c && d && c->samples != d->samples, "random LFO phase gives a repeatability floor");
}

void test_failures(Instance& inst) {
    auto job = make_job(inst);
    set_plain(inst, job.coords, ts::kHazNan, 1.0);
    auto nan = inst.render(job);
    check(!nan && nan.error() == Outcome::NonFinite, "NaN hazard gives NonFinite");

    // State poisoning: the filter stays NaN until the instance is re-activated; the next render must be clean.
    auto poison = make_job(inst);
    set_plain(inst, poison.coords, ts::kHazPoison, 1.0);
    auto p = inst.render(poison);
    check(!p && p.error() == Outcome::NonFinite, "poison hazard gives NonFinite");
    auto clean = inst.render(make_job(inst));
    check(clean.has_value(), "the render after a poisoned one is clean (reset between jobs)");

    Job none = make_job(inst);
    none.notes.clear();
    auto silent = inst.render(none);
    check(!silent && silent.error() == Outcome::Silent, "no notes gives Silent");
    none.allow_silent = true;
    check(inst.render(none).has_value(), "allow_silent accepts a silent target");

    Job bad = make_job(inst);
    bad.coords.pop_back();
    auto wrong = inst.render(bad);
    check(!wrong && wrong.error() == Outcome::HostError, "a short parameter vector is a HostError");
}

void test_pinning(Instance& inst) {
    // Bypass and program change are forced to their default, whatever the candidate says.
    auto job = make_job(inst);
    job.coords[index_of(inst, ts::kBypass)] = 1.0;
    job.coords[index_of(inst, ts::kProgram)] = 1.0;
    job.coords[index_of(inst, ts::kMeter)] = 1.0;
    auto a = inst.render(job);
    check(a.has_value(), "bypass cannot mute the plugin");
    auto ref = inst.render(make_job(inst));
    check(a && ref && a->samples == ref->samples, "pinned parameters do not change the sound");
}

void test_restart_and_state(Instance& inst, const std::string& path) {
    auto job = make_job(inst);
    set_plain(inst, job.coords, ts::kRestart, 1.0);
    const auto version = inst.parameters_version();
    check(inst.render(job).has_value(), "render that requests a restart succeeds");
    check(inst.render(make_job(inst)).has_value(), "render after the restart succeeds");
    check(inst.parameters_version() == version, "a restart without a table change keeps the version");

    // State round trip into a second instance.
    auto tuned = make_job(inst);
    set_plain(inst, tuned.coords, ts::kCutoff, 900);
    set_plain(inst, tuned.coords, ts::kWave, 1);
    auto blob = inst.save_state();
    check(blob.has_value() && !blob->empty(), "save_state returns a blob");
    auto other = open(path);
    check(other.has_value(), "second instance opens");
    if (!other || !blob) return;
    check((*other)->load_state(*blob).has_value(), "load_state accepts the blob");
    check(!(*other)->load_state({1, 2, 3}).has_value(), "load_state rejects garbage");
}

// T13: the same synth as CLAP and as VST3 gives the same parameters and (nearly) the same audio.
void test_equivalence(Instance& a, Instance& b) {
    const auto& pa = a.parameters();
    const auto& pb = b.parameters();
    check(pa.size() == pb.size(), "same parameter count in both formats");
    if (pa.size() != pb.size()) return;
    for (std::size_t i = 0; i < pa.size(); ++i) {
        check(pa[i].id == pb[i].id && pa[i].name == pb[i].name && pa[i].module == pb[i].module,
              "same id/name/module for " + pa[i].name);
        check(pa[i].units == pb[i].units, "same units for " + pa[i].name + ": '" + pa[i].units + "' vs '" + pb[i].units + "'");
        check(pa[i].flags.hidden == pb[i].flags.hidden && pa[i].flags.readonly == pb[i].flags.readonly &&
                  pa[i].flags.bypass == pb[i].flags.bypass && pa[i].flags.program_change == pb[i].flags.program_change,
              "same flags for " + pa[i].name);
    }
    auto ja = make_job(a, 0, 500), jb = make_job(b, 0, 500);
    for (Job* j : {&ja, &jb}) {
        Instance& inst = j == &ja ? a : b;
        set_plain(inst, j->coords, ts::kCutoff, 2000);
        set_plain(inst, j->coords, ts::kAttack, 20);
        set_plain(inst, j->coords, ts::kWave, 1);
    }
    auto ra = a.render(ja), rb = b.render(jb);
    check(ra && rb, "both formats render");
    if (!ra || !rb) return;
    const std::size_t n = std::min(ra->samples.size(), rb->samples.size());
    double err = 0, ref = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const double d = static_cast<double>(ra->samples[i]) - static_cast<double>(rb->samples[i]);
        err += d * d;
        ref += static_cast<double>(ra->samples[i]) * static_cast<double>(ra->samples[i]);
    }
    check(ref > 0 && std::sqrt(err / ref) < 0.02, "audio matches across formats: relative error " + std::to_string(std::sqrt(err / ref)));
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fputs("usage: mimicry_host_test <plugin>\n", stderr);
        return 2;
    }
    const std::string path = argv[1];
    auto inst = open(path);
    if (!inst) {
        std::fprintf(stderr, "FAIL: open(%s): %s\n", path.c_str(), inst.error().message.c_str());
        return 1;
    }
    test_enumeration(**inst);
    test_rendering(**inst);
    test_repeatability(**inst);
    test_failures(**inst);
    test_pinning(**inst);
    test_restart_and_state(**inst, path);

    if (argc >= 3) {
        auto other = open(argv[2]);
        check(other.has_value(), "second format opens");
        if (other) test_equivalence(**inst, **other);
    }

    check(!open("/nonexistent/x.clap").has_value(), "a missing file is an error");
    check(!open(path + ".nope").has_value(), "a wrong path is an error");

    if (failures == 0) std::puts("mimicry_host_test: all checks passed");
    return failures == 0 ? 0 : 1;
}
