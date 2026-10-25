#pragma once
// diff.ppm (docs/spec/07_OUTPUT_VALIDATION.md section 1) [S: Tannhauser's GUI snapshots are PPM too]: three
// log-frequency spectrograms stacked, target, match and the difference in dB (match minus target, blue where
// the match is quieter, red where it is louder), P6 binary.

#include <cstdint>
#include <vector>

namespace mimicry::output {

struct Image {
    int width = 0, height = 0;
    std::vector<std::uint8_t> rgb;  // width * height * 3
};

// `match` is scaled by `match_gain_db` first (the solved gain, so both are compared at the same level).
[[nodiscard]] Image spectrogram_comparison(const std::vector<float>& target, const std::vector<float>& match, int sample_rate,
                                           double match_gain_db = 0, int columns = 480, int rows = 160);

[[nodiscard]] std::vector<std::uint8_t> ppm_bytes(const Image& image);
// The same image as a 24-bit BMP (diff.bmp): Windows shows it without a viewer for PPM, which the front end needs.
[[nodiscard]] std::vector<std::uint8_t> bmp_bytes(const Image& image);

}  // namespace mimicry::output
