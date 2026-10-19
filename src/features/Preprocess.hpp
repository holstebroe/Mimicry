#pragma once
// Target pre-processing (docs/spec/03_FEATURES_LOSS.md section 1): mono mix, resampling, DC removal.

#include <vector>

namespace mimicry::features {

[[nodiscard]] std::vector<float> mix_to_mono(const std::vector<float>& interleaved, int channels);

// Band-limited resampling (Blackman-windowed sinc, 32 zero crossings per side).
[[nodiscard]] std::vector<float> resample(const std::vector<float>& x, int from_rate, int to_rate);

// Subtracts the mean of the pre-note silence from the whole signal, when there are at least 10 ms of it.
void remove_dc(std::vector<float>& x, int sample_rate);

}  // namespace mimicry::features
