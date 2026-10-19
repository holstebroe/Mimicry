#pragma once
// Target analysis, weights and the loss (docs/spec/03_FEATURES_LOSS.md sections 1-4, 6, 7; target
// fields of docs/spec/06_TARGETS.md section 1). All errors are in dB, lower is better.

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/Expected.hpp"
#include "core/Wav.hpp"
#include "features/Features.hpp"

namespace mimicry::features {

enum class SampleKind { Note, Hit, Texture, Loop };
[[nodiscard]] std::string_view to_string(SampleKind kind) noexcept;
[[nodiscard]] std::optional<SampleKind> parse_kind(std::string_view text);

enum class Feature { Msstft, Harm, Inter, Harmt, Env, Bands, Shape };
inline constexpr std::size_t kFeatureCount = 7;
[[nodiscard]] std::string_view to_string(Feature feature) noexcept;

struct Weights {
    std::array<double, kFeatureCount> w{};

    [[nodiscard]] static Weights preset(SampleKind kind);  // table of spec 03 section 3 [D]
    // `--w-<feature> N` override; false for an unknown name.
    bool set(std::string_view name, double value);
    [[nodiscard]] double operator[](Feature f) const { return w[std::size_t(f)]; }
};

struct AnalyzeOptions {
    int sample_rate = 48000;              // render rate; the target is resampled to it
    std::optional<int> key;               // --note override: the key; cents are then measured
    std::optional<SampleKind> kind;       // --kind override
};

struct TargetAnalysis {
    SampleKind kind = SampleKind::Texture;
    double f0 = 0;          // Hz, 0 when unpitched
    double confidence = 0;  // f0 confidence 0..1
    int key = 60;
    double cents = 0;       // tuning offset to apply to `key`
    double onset_ms = 0;
    double duration_ms = 0;  // onset to the last envelope frame within 40 dB of the peak
    double attack_ms = 0;    // peak-30 dB to peak-3 dB
    double floor_db = 0;     // noise floor: 5th percentile of the 1 ms envelope
    int onsets = 0;          // number of separate onsets (loop detection)
};

// A pre-processed, analysed target with its features and noise floors, computed once.
class Target {
public:
    [[nodiscard]] static expected<Target, std::string> from_wav(const WavData& wav, const AnalyzeOptions& opt);
    [[nodiscard]] static expected<Target, std::string> from_mono(std::vector<float> mono, int sample_rate,
                                                                 const AnalyzeOptions& opt);

    [[nodiscard]] const TargetAnalysis& analysis() const noexcept { return analysis_; }
    [[nodiscard]] const FeatureConfig& config() const noexcept { return config_; }
    [[nodiscard]] const std::vector<float>& mono() const noexcept { return mono_; }
    [[nodiscard]] std::size_t length() const noexcept { return mono_.size(); }
    [[nodiscard]] const FeatureSet& features() const noexcept { return features_; }

    // Lower clamps (noise floors and dynamic range), shared with the scorer.
    struct Floors {
        std::array<std::vector<float>, 3> msstft;  // per bin
        std::vector<float> bands;                  // per band
        std::vector<float> harm;                   // per harmonic
        float env = kFloorDb, harmt = kFloorDb, inter = kFloorDb;
    };
    [[nodiscard]] const Floors& floors() const noexcept { return floors_; }

private:
    TargetAnalysis analysis_;
    FeatureConfig config_;
    std::vector<float> mono_;
    FeatureSet features_;
    Floors floors_;
    friend class Scorer;
};

struct ScorerOptions {
    bool solve_gain = true;          // false when the plugin has a level parameter (spec 03 section 4)
    double dynamic_range_db = 90;    // nothing below (target peak - this) is scored [D]
    long offset_samples = 0;         // fitted global onset offset: >0 delays the candidate
};

struct Score {
    double cost = 0;      // sum(w_f * loss_f) / sum(w_f) over the active features, dB
    std::array<double, kFeatureCount> loss{};
    double gain_db = 0;   // dB the candidate needs to match the target level
};

class Scorer {
public:
    Scorer(const Target& target, Weights weights, ScorerOptions options = {});

    [[nodiscard]] const Weights& weights() const noexcept { return weights_; }
    void set_offset(long samples) { options_.offset_samples = samples; }
    [[nodiscard]] long offset() const noexcept { return options_.offset_samples; }

    // Samples to delay (positive) or advance (negative) the candidate so its onset meets the target's.
    [[nodiscard]] long estimate_offset(const std::vector<float>& candidate_mono) const;

    // `candidate_mono` is shifted by the offset, then padded or cut to the target's length.
    [[nodiscard]] Score score(const std::vector<float>& candidate_mono);

private:
    const Target& target_;
    Weights weights_;
    ScorerOptions options_;
    FeatureExtractor extractor_;
    std::array<std::vector<float>, 3> lo_msstft_;
    std::vector<float> lo_bands_, lo_harm_;
    float lo_env_ = kFloorDb, lo_harmt_ = kFloorDb, lo_inter_ = kFloorDb;
    std::vector<float> clamped_inter_;
    std::array<Grid, 3> t_msstft_;
    Grid t_bands_, t_harmt_, t_env_;
    std::vector<float> t_harm_;
    std::vector<double> harm_weight_;
};

// Repeatability (spec 03 section 6): the cost between two renders of the same preset.
struct RepeatPolicy {
    double floor_db = 0;
    int renders = 1;           // 1 if the floor is below 0.5 dB, else up to 4
    double success_db = 0;     // 1.5 x floor
};
[[nodiscard]] RepeatPolicy repeat_policy(double floor_db);

// Cost between two mono renders of the same preset, scored with `weights` (gain solved). The first render
// plays the part of the target, so it is analysed like one.
[[nodiscard]] double repeatability_floor_db(const std::vector<float>& a, const std::vector<float>& b,
                                            int sample_rate, const Weights& weights);

}  // namespace mimicry::features
