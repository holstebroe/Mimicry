#pragma once
// WAV reading and writing (RIFF PCM 16/24/32 bit and IEEE float 32/64, plus WAVE_FORMAT_EXTENSIBLE).
// Samples are interleaved float. Own implementation, no third-party code.

#include <filesystem>
#include <string>
#include <vector>

#include "core/Expected.hpp"

namespace mimicry {

struct WavData {
    int sample_rate = 0;
    int channels = 0;
    std::vector<float> samples;  // interleaved
    [[nodiscard]] std::size_t frames() const {
        return channels > 0 ? samples.size() / static_cast<std::size_t>(channels) : 0;
    }
};

[[nodiscard]] expected<WavData, std::string> read_wav(const std::filesystem::path& path);
[[nodiscard]] expected<WavData, std::string> parse_wav(const std::vector<std::uint8_t>& bytes);

enum class WavFormat { Pcm16, Pcm24, Float32 };

[[nodiscard]] std::vector<std::uint8_t> encode_wav(const WavData& wav, WavFormat format = WavFormat::Float32);
[[nodiscard]] expected<void, std::string> write_wav(const std::filesystem::path& path, const WavData& wav,
                                                    WavFormat format = WavFormat::Float32);

}  // namespace mimicry
