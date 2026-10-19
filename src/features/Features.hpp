#pragma once
// Feature extraction (docs/spec/03_FEATURES_LOSS.md section 2). All log-domain features are in dB of
// power and insensitive to waveform phase. A FeatureExtractor owns FFT plans and scratch space: create
// one per thread. Target and candidates must use the same FeatureConfig and the same signal length.

#include <array>
#include <cstddef>
#include <memory>
#include <vector>

#include "features/Spectral.hpp"

namespace mimicry::features {

inline constexpr std::array<std::size_t, 3> kMsstftSizes = {512, 2048, 8192};
inline constexpr std::size_t kShapeDims = 17;  // centroid, flatness, rolloff, flux, MFCC 1..13

struct FeatureConfig {
    int sample_rate = 48000;
    double f0 = 0;             // Hz; 0 = unpitched, which disables harm / inter / harmt
    int max_harmonics = 64;
};

// frames x cols, row-major.
struct Grid {
    std::size_t frames = 0, cols = 0;
    std::vector<float> v;
    [[nodiscard]] const float* row(std::size_t f) const { return v.data() + f * cols; }
    [[nodiscard]] float* row(std::size_t f) { return v.data() + f * cols; }
};

struct FeatureSet {
    std::size_t length = 0;
    int harmonics = 0;               // K; 0 when unpitched
    std::array<Grid, 3> msstft;      // dB, 512 / 2048 / 8192-point, hop N/4
    Grid env;                        // dB RMS, 1 ms hop, one column
    Grid bands;                      // dB per 1/3-octave band, ~23 ms frames
    Grid harmt;                      // dB per harmonic, 40 ms frames every 10 ms
    std::vector<float> harm;         // dB energy within +-f0/4 of k*f0 in the whole-note DFT
    std::vector<float> inter;        // dB energy between harmonic k and k+1 (k = 1..K)
    std::vector<float> shape;        // see kShapeDims
};

class FeatureExtractor {
public:
    explicit FeatureExtractor(const FeatureConfig& config);
    ~FeatureExtractor();
    FeatureExtractor(const FeatureExtractor&) = delete;
    FeatureExtractor& operator=(const FeatureExtractor&) = delete;

    [[nodiscard]] const FeatureConfig& config() const noexcept { return config_; }
    [[nodiscard]] int harmonics() const noexcept { return harmonics_; }

    // `mono` is zero padded or truncated to `length` samples first.
    [[nodiscard]] FeatureSet extract(const std::vector<float>& mono, std::size_t length);

private:
    struct Impl;
    FeatureConfig config_;
    int harmonics_ = 0;
    std::unique_ptr<Impl> impl_;
};

// Short-time RMS envelope in dB: `window` samples, `hop` samples.
[[nodiscard]] std::vector<float> rms_envelope_db(const std::vector<float>& x, std::size_t window, std::size_t hop);

// First frame of an envelope that rises above max(floor + 12 dB, peak - 50 dB), but never above peak - 6 dB
// (steady signals); in frames, or -1 for silence. `floor` is the 5th percentile of the envelope.
[[nodiscard]] long onset_frame(const std::vector<float>& env_db);

}  // namespace mimicry::features
