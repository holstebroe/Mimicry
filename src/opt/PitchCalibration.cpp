#include "opt/PitchCalibration.hpp"

#include <cmath>

#include "features/Pitch.hpp"

namespace mimicry::opt {

namespace {

constexpr double kMinConfidence = 0.5;  // a calibration render must be clearly pitched [D]

double f0_of(const host::Audio& audio, double* confidence = nullptr) {
    const auto e = features::estimate_f0(mono_of(audio), audio.sample_rate);
    if (confidence) *confidence = e.confidence;
    return e.f0;
}

double cents_between(double from, double to) { return from > 0 && to > 0 ? 1200.0 * std::log2(to / from) : 0.0; }

}  // namespace

std::string_view to_string(PitchMethod m) noexcept {
    switch (m) {
        case PitchMethod::Tuning: return "tuning";
        case PitchMethod::Bend: return "bend";
        case PitchMethod::NearestKey: return "nearest_key";
        case PitchMethod::Unknown: break;
    }
    return "unknown";
}

PitchMethod parse_pitch_method(std::string_view text) noexcept {
    for (auto m : {PitchMethod::Tuning, PitchMethod::Bend, PitchMethod::NearestKey})
        if (to_string(m) == text) return m;
    return PitchMethod::Unknown;
}

PitchCalibration calibrate_pitch(const RenderBatch& render, host::Job base) {
    PitchCalibration c;
    if (base.notes.empty()) {
        c.note = "no note to calibrate with";
        return c;
    }
    base.bend_range = 0;
    for (auto& n : base.notes) n.cents = 0;
    auto tuned = base;
    for (auto& n : tuned.notes) n.cents = 50;
    auto bent = base;  // bend range 1 and +100 cents: a full bend up, whatever the plugin's real range
    bent.bend_range = 1;
    for (auto& n : bent.notes) n.cents = 100;
    const auto got = render({base, tuned, bent});
    if (got.size() < 3 || !got[0]) {
        c.note = "the calibration note did not render";
        return c;
    }
    double conf = 0;
    const double f_plain = f0_of(*got[0], &conf);
    if (f_plain <= 0 || conf < kMinConfidence) {
        c.note = "the calibration note is not clearly pitched";
        return c;
    }
    if (got[1]) c.tuning_cents = cents_between(f_plain, f0_of(*got[1]));
    if (got[2]) c.bend_cents = cents_between(f_plain, f0_of(*got[2]));
    if (std::abs(c.tuning_cents - 50.0) <= 10.0) {
        c.method = PitchMethod::Tuning;
    } else if (c.bend_cents >= 20.0) {
        c.method = PitchMethod::Bend;
        c.bend_range = c.bend_cents / 100.0;
    } else {
        c.method = PitchMethod::NearestKey;
    }
    return c;
}

void apply_pitch_method(const PitchCalibration& c, host::Job& job) {
    if (c.method == PitchMethod::Bend && c.bend_range > 0) {
        job.bend_range = c.bend_range;
    } else if (c.method == PitchMethod::NearestKey) {
        job.bend_range = 0;
        for (auto& n : job.notes) n.cents = 0;
    }
}

double pitch_residual_cents(const host::Audio& audio, double target_f0) {
    double conf = 0;
    const double f = f0_of(audio, &conf);
    if (conf < kMinConfidence) return 0;
    return cents_between(target_f0, f);
}

}  // namespace mimicry::opt
