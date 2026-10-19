#pragma once
// Compact feature embedding of a render (docs/spec/04_OPTIMIZER.md section 7): used by the probe's
// signatures and pair probes, the top-K diversity in feature space and the fingerprint database.
//
// Layout (all dB-like, so one Euclidean distance makes sense) [D]:
//   [0, 32)   mean log-mel band level over active frames, relative to the mean over all bands (level free)
//   [32, 64)  standard deviation of each band over active frames
//   64, 65    centroid (6 x log2 Hz) mean and standard deviation
//   66, 67    flatness (dB) mean and standard deviation
//   [68, 76)  envelope at 10, 25, 50, 100, 200, 400, 800, 1600 ms after the onset, dB relative to the peak
//   76        overall level (dB RMS of the active frames); left out of distance()
// Not thread-safe: create one Embedder per thread.

#include <array>
#include <cstddef>
#include <memory>
#include <vector>

namespace mimicry::opt {

inline constexpr std::size_t kEmbeddingDims = 77;
inline constexpr std::size_t kEmbeddingLevel = 76;
inline constexpr std::size_t kEmbeddingCentroid = 64;
inline constexpr std::size_t kEmbeddingFlatness = 66;
inline constexpr std::size_t kEmbeddingEnvelope = 68;
inline constexpr std::array<double, 8> kEmbeddingEnvelopeMs = {10, 25, 50, 100, 200, 400, 800, 1600};

using Embedding = std::vector<float>;

class Embedder {
public:
    explicit Embedder(int sample_rate);
    ~Embedder();
    Embedder(const Embedder&) = delete;
    Embedder& operator=(const Embedder&) = delete;

    [[nodiscard]] int sample_rate() const noexcept { return rate_; }
    [[nodiscard]] Embedding embed(const std::vector<float>& mono);

private:
    struct Impl;
    int rate_;
    std::unique_ptr<Impl> impl_;
};

// RMS over every dimension but the level.
[[nodiscard]] double embedding_distance(const Embedding& a, const Embedding& b);

}  // namespace mimicry::opt
