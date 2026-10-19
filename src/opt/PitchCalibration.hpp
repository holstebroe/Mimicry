#pragma once
// How a plugin takes fine pitch (docs/spec/02_HOSTING.md section 5): the format's tuning (CLAP note
// expression, VST3 NoteOnEvent.tuning), MIDI pitch bend with a measured range, or only the nearest key.
// Three calibration renders decide; the result goes into the plugin profile.

#include <string>
#include <string_view>

#include "core/Expected.hpp"
#include "host/PluginHost.hpp"
#include "opt/Objective.hpp"

namespace mimicry::opt {

enum class PitchMethod { Unknown, Tuning, Bend, NearestKey };
[[nodiscard]] std::string_view to_string(PitchMethod m) noexcept;
[[nodiscard]] PitchMethod parse_pitch_method(std::string_view text) noexcept;

struct PitchCalibration {
    PitchMethod method = PitchMethod::Unknown;
    double bend_range = 0;     // semitones, for Bend
    double tuning_cents = 0;   // measured shift of a +50 cent tuning request
    double bend_cents = 0;     // measured shift of a full bend
    std::string note;          // why Unknown
};

// Renders `base` (one note, cents ignored) three times: plain, +50 cents through the format's tuning, and a full
// MIDI pitch bend up; compares the f0 of each. Tuning wins when it moves the pitch within 10 cents of the
// request [D]; else bend when a full bend moves it at least 20 cents (range = that shift) [D]; else the
// nearest key. Unknown when the plain render is not clearly pitched.
[[nodiscard]] PitchCalibration calibrate_pitch(const RenderBatch& render, host::Job base);

// Applies a calibration to a job's notes: bend range for Bend, cents dropped for NearestKey (the key is
// already the nearest); Tuning and Unknown leave the job as it is.
void apply_pitch_method(const PitchCalibration& c, host::Job& job);

// Measured pitch difference between a render and a target f0, in cents (0 when either is unpitched).
[[nodiscard]] double pitch_residual_cents(const host::Audio& audio, double target_f0);

}  // namespace mimicry::opt
