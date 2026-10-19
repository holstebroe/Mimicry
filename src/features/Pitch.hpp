#pragma once
// f0 estimation and note/cents conversion (docs/spec/06_TARGETS.md section 1, 02_HOSTING.md section 5).
// Pitch is measured from the target and applied to the notes directly, never searched.

#include <optional>
#include <string_view>
#include <vector>

namespace mimicry::features {

struct PitchEstimate {
    double f0 = 0;          // Hz, 0 when nothing pitched was found
    double confidence = 0;  // 0..1: share of spectral energy that sits on the harmonic comb
};

// Harmonic-comb search over the strongest spectral peaks, refined by weighted least squares on the
// parabolic-interpolated peak frequencies [S: Acidus `estimate_f0`, re-derived].
[[nodiscard]] PitchEstimate estimate_f0(const std::vector<float>& mono, int sample_rate, double fmin = 30,
                                        double fmax = 2000);

struct NotePitch {
    int key = 60;
    double cents = 0;  // in [-50, 50]
};

[[nodiscard]] NotePitch note_from_hz(double hz);
[[nodiscard]] double hz_from_note(int key, double cents = 0);

// "C3", "F#2", "Bb4" or a plain MIDI number. C4 = 60 (A4 = 440 Hz) [D].
[[nodiscard]] std::optional<int> parse_note_name(std::string_view text);

}  // namespace mimicry::features
