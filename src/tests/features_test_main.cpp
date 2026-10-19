// Tests for WP2: pitch, features, loss, weights, repeatability and the render cache. Covers T4 (feature
// invariance, docs/spec/07_OUTPUT_VALIDATION.md) for every feature of spec 03.

#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "core/Rng.hpp"
#include "features/Features.hpp"
#include "features/Loss.hpp"
#include "features/Pitch.hpp"
#include "features/Preprocess.hpp"
#include "opt/RenderCache.hpp"

using namespace mimicry;
using namespace mimicry::features;

namespace {

int failures = 0;
constexpr double kPi = 3.141592653589793;
constexpr int kRate = 48000;

void check(bool ok, const std::string& what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
        ++failures;
    }
}

// Additive tone: harmonics 1..n with amplitude k^-1, random phases (seed), exponential decay, a short
// pre-roll of silence, optional white noise floor.
std::vector<float> tone(double f0, double seconds, std::uint64_t phase_seed, int harmonics = 12, double pre_ms = 50,
                        double decay_s = 0.8, double noise = 0.0, std::uint64_t noise_seed = 5) {
    Rng ph(phase_seed), nz(noise_seed);
    std::vector<double> phase(std::size_t(harmonics) + 1);
    for (auto& p : phase) p = phase_seed ? ph.uniform(0, 2 * kPi) : 0.0;
    const std::size_t n = std::size_t(seconds * kRate), pre = std::size_t(pre_ms * kRate / 1000);
    std::vector<float> x(n, 0.0f);
    for (std::size_t i = pre; i < n; ++i) {
        const double t = double(i - pre) / kRate;
        double v = 0;
        for (int k = 1; k <= harmonics; ++k)
            if (k * f0 < 0.45 * kRate) v += std::sin(2 * kPi * k * f0 * t + phase[std::size_t(k)]) / k;
        const double env = std::min(1.0, t / 0.005) * std::exp(-t / decay_s);
        x[i] = float(0.2 * v * env);
    }
    if (noise > 0)
        for (auto& s : x) s += float(noise * nz.normal());
    return x;
}

std::vector<float> noise_burst(double seconds, double decay_s, std::uint64_t seed, double pre_ms = 50) {
    Rng r(seed);
    const std::size_t n = std::size_t(seconds * kRate), pre = std::size_t(pre_ms * kRate / 1000);
    std::vector<float> x(n, 0.0f);
    for (std::size_t i = pre; i < n; ++i) x[i] = float(0.3 * r.normal() * std::exp(-double(i - pre) / kRate / decay_s));
    return x;
}

std::vector<float> scaled(std::vector<float> x, double g) {
    for (auto& v : x) v = float(v * g);
    return x;
}

std::vector<float> delayed(const std::vector<float>& x, std::size_t d) {
    std::vector<float> y(x.size(), 0.0f);
    for (std::size_t i = d; i < x.size(); ++i) y[i] = x[i - d];
    return y;
}

Target make_target(const std::vector<float>& x, std::optional<SampleKind> kind = std::nullopt) {
    AnalyzeOptions opt;
    opt.sample_rate = kRate;
    opt.kind = kind;
    auto t = Target::from_mono(x, kRate, opt);
    if (!t) {
        std::fprintf(stderr, "Target failed: %s\n", t.error().c_str());
        std::exit(2);
    }
    return std::move(*t);
}

void test_pitch() {
    for (double f : {110.0, 261.6256, 440.0, 987.77}) {
        const auto e = estimate_f0(tone(f, 1.5, 3), kRate);
        check(std::fabs(e.f0 / f - 1.0) < 0.001, "f0 of a harmonic tone: " + std::to_string(f) + " got " + std::to_string(e.f0));
        check(e.confidence > 0.8, "f0 confidence of a clean tone");
    }
    // Odd harmonics only (square-like) must not collapse to a sub-octave comb.
    {
        Rng r(1);
        std::vector<float> x(kRate);
        for (std::size_t i = 0; i < x.size(); ++i) {
            double v = 0;
            for (int k = 1; k < 20; k += 2) v += std::sin(2 * kPi * k * 200.0 * double(i) / kRate) / k;
            x[i] = float(0.2 * v);
        }
        const auto e = estimate_f0(x, kRate);
        check(std::fabs(e.f0 / 200.0 - 1.0) < 0.002, "f0 of an odd-harmonic tone: " + std::to_string(e.f0));
    }
    // Tuning: 25 cents sharp of A3.
    {
        const double f = 220.0 * std::pow(2.0, 25.0 / 1200.0);
        const auto np = note_from_hz(estimate_f0(tone(f, 1.5, 9), kRate).f0);
        check(np.key == 57 && std::fabs(np.cents - 25.0) < 1.0, "key and cents: " + std::to_string(np.key) + " " + std::to_string(np.cents));
    }
    const auto n = estimate_f0(noise_burst(1.0, 5.0, 4), kRate);
    check(n.f0 == 0 || n.confidence < 0.5, "noise is not pitched: " + std::to_string(n.confidence));
    check(estimate_f0(std::vector<float>(kRate, 0.0f), kRate).f0 == 0, "silence has no pitch");

    check(note_from_hz(440).key == 69 && std::fabs(note_from_hz(440).cents) < 1e-9, "A4 is key 69");
    check(std::fabs(hz_from_note(60) - 261.6256) < 1e-3, "C4 frequency");
    check(parse_note_name("C4") == 60 && parse_note_name("c3") == 48 && parse_note_name("F#2") == 42 &&
              parse_note_name("Bb4") == 70 && parse_note_name("64") == 64 && !parse_note_name("H2") && !parse_note_name("C") &&
              !parse_note_name("C4x") && !parse_note_name("200"),
          "note names");
}

void test_preprocess() {
    std::vector<float> s(44100);
    for (std::size_t i = 0; i < s.size(); ++i) s[i] = float(0.5 * std::sin(2 * kPi * 1000.0 * double(i) / 44100.0));
    const auto r = resample(s, 44100, 48000);
    check(std::abs(long(r.size()) - 48000) <= 1, "resample length");
    double err = 0;
    for (std::size_t i = 2000; i < 46000; ++i) err = std::max(err, std::fabs(double(r[i]) - 0.5 * std::sin(2 * kPi * 1000.0 * double(i) / 48000.0)));
    check(err < 0.01, "resample accuracy: " + std::to_string(err));
    const auto down = resample(r, 48000, 22050);
    check(std::abs(long(down.size()) - 22050) <= 1, "downsample length");

    const std::vector<float> st = {1, 3, -1, 1, 2, 2};
    const auto m = mix_to_mono(st, 2);
    check(m.size() == 3 && m[0] == 2 && m[1] == 0 && m[2] == 2, "mix to mono");

    auto x = tone(220, 1.0, 2, 8, 100);
    for (auto& v : x) v += 0.005f;
    remove_dc(x, kRate);
    double mean = 0;
    for (std::size_t i = 0; i < 2000; ++i) mean += x[i];
    check(std::fabs(mean / 2000) < 1e-3, "DC removed using the pre-note silence");
}

void test_analysis_and_kinds() {
    const auto note = make_target(tone(220, 2.0, 7));
    const auto& a = note.analysis();
    check(a.kind == SampleKind::Note, std::string("tone is a note, got ") + std::string(to_string(a.kind)));
    check(a.key == 57 && std::fabs(a.cents) < 1.0, "target key and cents");
    check(std::fabs(a.onset_ms - 50) <= 3, "onset found at 50 ms: " + std::to_string(a.onset_ms));
    check(a.duration_ms > 1000, "duration measured");
    check(note.features().harmonics >= 20, "harmonics counted");

    const auto hit = make_target(noise_burst(1.0, 0.08, 3));
    check(hit.analysis().kind == SampleKind::Hit, std::string("noise burst is a hit, got ") + std::string(to_string(hit.analysis().kind)));
    const auto tex = make_target(noise_burst(3.0, 100.0, 3));
    check(tex.analysis().kind == SampleKind::Texture, std::string("steady noise is a texture, got ") + std::string(to_string(tex.analysis().kind)));

    // Loop: four clicks a second apart.
    std::vector<float> loop(4 * kRate, 0.0f);
    Rng r(2);
    for (int b = 0; b < 4; ++b)
        for (int i = 0; i < 2400; ++i) loop[std::size_t(b * kRate + 500 + i)] = float(0.4 * r.normal() * std::exp(-i / 600.0));
    const auto lp = make_target(loop);
    check(lp.analysis().kind == SampleKind::Loop, std::string("click train is a loop, got ") + std::string(to_string(lp.analysis().kind)) + " onsets " + std::to_string(lp.analysis().onsets));

    AnalyzeOptions o;
    o.sample_rate = kRate;
    o.key = 58;
    o.kind = SampleKind::Texture;
    auto forced = Target::from_mono(tone(220, 1.0, 7), kRate, o);
    check(forced && forced->analysis().key == 58 && forced->analysis().kind == SampleKind::Texture &&
              std::fabs(forced->analysis().cents + 50.0) < 1e-9, "--note and --kind overrides (cents clamp)");

    auto w = Weights::preset(SampleKind::Note);
    check(w[Feature::Harm] == 1 && w[Feature::Shape] == 0 && w[Feature::Inter] == 0.5, "note weights");
    check(Weights::preset(SampleKind::Hit)[Feature::Env] == 1.5 && Weights::preset(SampleKind::Texture)[Feature::Shape] == 1 &&
              Weights::preset(SampleKind::Loop)[Feature::Shape] == 0.5, "other weight presets");
    check(w.set("harmt", 2.5) && w[Feature::Harmt] == 2.5 && !w.set("nope", 1), "--w-<feature> override");
    check(parse_kind("loop") == SampleKind::Loop && !parse_kind("x"), "kind names");
}

// T4: gain shifts only gain_db; time shift shifts only the onset; phase leaves msstft, harm, bands alone.
void test_invariance() {
    const auto base = tone(220, 2.0, 11);
    const auto target = make_target(base);
    Scorer scorer(target, Weights::preset(SampleKind::Note));

    const auto self = scorer.score(base);
    check(self.cost < 1e-6 && std::fabs(self.gain_db) < 1e-6, "self score is zero");
    for (std::size_t i = 0; i < kFeatureCount; ++i) check(self.loss[i] < 1e-6, "self loss, feature " + std::to_string(i));

    for (double db : {-12.0, -3.0, 6.0}) {
        const auto s = scorer.score(scaled(base, std::pow(10.0, db / 20.0)));
        check(std::fabs(s.gain_db + db) < 0.05, "gain_db reports the level offset " + std::to_string(db) + ": " + std::to_string(s.gain_db));
        check(s.cost < 0.05, "gain change leaves the cost alone: " + std::to_string(s.cost));
    }
    {
        // With a free level parameter the gain is not solved, so a level error is a cost.
        ScorerOptions opt;
        opt.solve_gain = false;
        Scorer raw(target, Weights::preset(SampleKind::Note), opt);
        check(raw.score(scaled(base, 0.5)).cost > 2.0 && raw.score(scaled(base, 0.5)).gain_db == 0, "gain not solved when a level parameter exists");
    }

    // Phase: other random phases of the same harmonics.
    const auto phased = scorer.score(tone(220, 2.0, 99));
    check(phased.loss[std::size_t(Feature::Msstft)] < 0.5, "phase: msstft " + std::to_string(phased.loss[std::size_t(Feature::Msstft)]));
    check(phased.loss[std::size_t(Feature::Harm)] < 0.2, "phase: harm " + std::to_string(phased.loss[std::size_t(Feature::Harm)]));
    check(phased.loss[std::size_t(Feature::Bands)] < 0.5, "phase: bands " + std::to_string(phased.loss[std::size_t(Feature::Bands)]));
    check(phased.loss[std::size_t(Feature::Harmt)] < 0.5, "phase: harmt " + std::to_string(phased.loss[std::size_t(Feature::Harmt)]));
    check(phased.loss[std::size_t(Feature::Shape)] < 0.5, "phase: shape " + std::to_string(phased.loss[std::size_t(Feature::Shape)]));
    const auto inverted = scorer.score(scaled(base, -1.0));
    check(inverted.cost < 1e-3, "polarity inversion changes nothing");

    // Time: a 20 ms late candidate is detected as an onset offset and, once applied, scores like the target.
    const auto late = delayed(base, 960);
    const long off = scorer.estimate_offset(late);
    check(std::labs(off + 960) <= 48, "onset offset estimated: " + std::to_string(off));
    const double unaligned = scorer.score(late).cost;
    scorer.set_offset(off);
    const double aligned = scorer.score(late).cost;
    check(aligned < 0.3 && unaligned > aligned, "aligned onset: " + std::to_string(aligned) + " vs " + std::to_string(unaligned));
    scorer.set_offset(0);

    // A different pitch and a different timbre must cost something.
    check(scorer.score(tone(233.08, 2.0, 11)).cost > 3.0, "a different pitch is penalised");
    check(scorer.score(tone(220, 2.0, 11, 2)).loss[std::size_t(Feature::Harm)] > 3.0, "missing harmonics are penalised");
    check(scorer.score(tone(220, 2.0, 11, 12, 50, 0.1)).loss[std::size_t(Feature::Env)] > 3.0, "a faster decay is penalised in env");
    check(scorer.score(std::vector<float>(base.size(), 0.0f)).cost > 10.0, "silence scores badly");
    check(scorer.score(std::vector<float>(100, 0.1f)).cost > 1.0, "a short candidate is padded, not rejected");
}

void test_noise_floor_clamp() {
    // The target is a tone on a noisy floor; the candidate is the clean tone.
    const auto noisy = tone(220, 2.0, 5, 12, 50, 0.8, 0.003);
    const auto clean = tone(220, 2.0, 5);
    const auto target = make_target(noisy, SampleKind::Note);
    Scorer scorer(target, Weights::preset(SampleKind::Note));
    const auto s = scorer.score(clean);
    check(s.loss[std::size_t(Feature::Inter)] < 1e-6, "inter counts only content added on top of the target's noise: " + std::to_string(s.loss[std::size_t(Feature::Inter)]));
    check(s.loss[std::size_t(Feature::Harm)] < 1.0, "harm clamped at the noise floor: " + std::to_string(s.loss[std::size_t(Feature::Harm)]));
    // The clean tone's harmonics above the noise still count: a tone with a spur between harmonics is worse.
    auto spur = clean;
    for (std::size_t i = 0; i < spur.size(); ++i) spur[i] += float(0.05 * std::sin(2 * kPi * 330.0 * double(i) / kRate) * std::exp(-double(i) / kRate / 0.8));
    const auto sp = scorer.score(spur);
    check(sp.loss[std::size_t(Feature::Inter)] > 0.3, "a spur between harmonics is penalised: " + std::to_string(sp.loss[std::size_t(Feature::Inter)]));

    // The reverse (clean target, noisy candidate) is penalised.
    const auto clean_target = make_target(clean, SampleKind::Note);
    Scorer s2(clean_target, Weights::preset(SampleKind::Note));
    check(s2.score(noisy).cost > s.cost + 0.5, "noise the target does not have is penalised");

    // Unpitched weights drop the harmonic features entirely.
    const auto hit = make_target(noise_burst(1.0, 0.08, 3));
    Scorer h(hit, Weights::preset(SampleKind::Note));
    check(h.weights()[Feature::Harm] == 0 && h.weights()[Feature::Harmt] == 0 && h.weights()[Feature::Inter] == 0, "harmonic features off without f0");
    check(h.score(noise_burst(1.0, 0.08, 4)).cost < h.score(noise_burst(1.0, 0.5, 4)).cost, "same decay scores better than a longer one");
}

void test_repeat() {
    check(repeat_policy(0.2).renders == 1 && std::fabs(repeat_policy(0.2).success_db - 0.3) < 1e-12, "repeat policy: stable plugin");
    const auto p = repeat_policy(1.6);
    check(p.renders >= 2 && p.renders <= 4 && std::fabs(p.success_db - 2.4) < 1e-12, "repeat policy: drifting plugin");
    check(repeat_policy(50).renders == 4, "repeat policy caps at 4 renders");

    const auto w = Weights::preset(SampleKind::Note);
    const auto a = tone(220, 1.5, 7);
    check(repeatability_floor_db(a, a, kRate, w) < 1e-6, "identical renders: zero floor");
    const double floor = repeatability_floor_db(a, tone(220, 1.5, 8, 12, 50, 0.8, 0.002, 77), kRate, w);
    check(floor > 0.01 && floor < 20, "different renders: a measurable floor " + std::to_string(floor));
}

void test_cost() {
    const auto base = tone(220, 2.0, 11);
    const auto target = make_target(base);
    Scorer scorer(target, Weights::preset(SampleKind::Note));
    const auto cand = tone(222, 2.0, 12);
    (void)scorer.score(cand);
    const auto t0 = std::chrono::steady_clock::now();
    const int reps = 20;
    for (int i = 0; i < reps; ++i) (void)scorer.score(cand);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / reps;
    std::printf("feature cost for a 2 s note: %.2f ms per candidate (target budget 5 ms)\n", ms);
    check(ms < 100, "feature cost is sane: " + std::to_string(ms));
}

void test_cache() {
    host::Job a;
    a.coords = {0.1, 0.2, 0.3};
    a.notes.push_back({60, 0, 0.8f, 0, 500});
    host::Job b = a;
    check(opt::RenderCache::key(a) == opt::RenderCache::key(b), "same job, same key");
    b.coords[1] = std::nextafter(0.2, 1.0);
    check(opt::RenderCache::key(a) != opt::RenderCache::key(b), "one ulp changes the key");
    b = a;
    b.block_size = 256;
    check(opt::RenderCache::key(a) != opt::RenderCache::key(b), "block size is part of the key");
    b = a;
    b.notes[0].cents = 3;
    check(opt::RenderCache::key(a) != opt::RenderCache::key(b), "note plan is part of the key");
    b = a;
    b.sample_rate = 44100;
    check(opt::RenderCache::key(a) != opt::RenderCache::key(b), "rate is part of the key");

    opt::RenderCache cache;
    int renders = 0;
    auto render = [&](const host::Job&) -> expected<host::Audio, Outcome> {
        ++renders;
        host::Audio au;
        au.sample_rate = kRate;
        au.channels = 1;
        au.samples.assign(100, 0.5f);
        return au;
    };
    auto msg = [] { return std::string(); };
    const auto r1 = cache.get_or_render(a, render, msg);
    const auto r2 = cache.get_or_render(a, render, msg);
    check(renders == 1 && r1.audio == r2.audio && cache.hits() == 1 && cache.misses() == 1, "duplicate candidates render once");

    int fails = 0;
    auto crash = [&](const host::Job&) -> expected<host::Audio, Outcome> {
        ++fails;
        return mimicry::unexpected(Outcome::Crash);
    };
    host::Job c = a;
    c.coords[0] = 0.9;
    auto f1 = cache.get_or_render(c, crash, [] { return std::string("segfault"); });
    auto f2 = cache.get_or_render(c, crash, [] { return std::string("again"); });
    check(fails == 1 && f1.outcome == Outcome::Crash && f2.message == "segfault", "a crash is remembered, not re-rendered");
    auto timeout = [&](const host::Job&) -> expected<host::Audio, Outcome> {
        ++fails;
        return mimicry::unexpected(Outcome::Timeout);
    };
    host::Job d = a;
    d.coords[0] = 0.5;
    (void)cache.get_or_render(d, timeout, msg);
    (void)cache.get_or_render(d, timeout, msg);
    check(fails == 3, "a timeout is retried");

    opt::RenderCache small(2000);  // fits about three of the 400-byte renders
    for (int i = 0; i < 10; ++i) {
        host::Job j = a;
        j.coords[0] = i;
        (void)small.get_or_render(j, render, msg);
    }
    check(small.size() < 10 && small.size() >= 1 && small.bytes() <= 2000 + 500, "least recently used renders are evicted");
}

}  // namespace

int main() {
    test_pitch();
    test_preprocess();
    test_analysis_and_kinds();
    test_invariance();
    test_noise_floor_clamp();
    test_repeat();
    test_cost();
    test_cache();
    if (failures == 0) std::puts("mimicry_features_test: all checks passed");
    return failures == 0 ? 0 : 1;
}
