#include "core/Wav.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <fstream>

namespace mimicry {

namespace {

std::uint32_t rd32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | static_cast<std::uint32_t>(p[1]) << 8 |
           static_cast<std::uint32_t>(p[2]) << 16 | static_cast<std::uint32_t>(p[3]) << 24;
}
std::uint16_t rd16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] | p[1] << 8); }

void wr32(std::vector<std::uint8_t>& o, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) o.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}
void wr16(std::vector<std::uint8_t>& o, std::uint16_t v) {
    o.push_back(static_cast<std::uint8_t>(v));
    o.push_back(static_cast<std::uint8_t>(v >> 8));
}
void tag(std::vector<std::uint8_t>& o, const char* t) { o.insert(o.end(), t, t + 4); }

}  // namespace

expected<WavData, std::string> parse_wav(const std::vector<std::uint8_t>& b) {
    if (b.size() < 12 || std::memcmp(b.data(), "RIFF", 4) != 0 || std::memcmp(b.data() + 8, "WAVE", 4) != 0)
        return unexpected(std::string("wav: not a RIFF/WAVE file"));

    int format_tag = 0, channels = 0, bits = 0;
    std::uint32_t rate = 0;
    bool have_fmt = false;
    const std::uint8_t* data = nullptr;
    std::size_t data_len = 0;

    std::size_t pos = 12;
    while (pos + 8 <= b.size()) {
        const std::uint8_t* hdr = b.data() + pos;
        std::size_t len = rd32(hdr + 4);
        const std::size_t body = pos + 8;
        len = std::min(len, b.size() - body);  // tolerate a truncated final chunk
        if (std::memcmp(hdr, "fmt ", 4) == 0 && len >= 16) {
            format_tag = rd16(b.data() + body);
            channels = rd16(b.data() + body + 2);
            rate = rd32(b.data() + body + 4);
            bits = rd16(b.data() + body + 14);
            if (format_tag == 0xFFFE && len >= 26) format_tag = rd16(b.data() + body + 24);  // extensible sub-format
            have_fmt = true;
        } else if (std::memcmp(hdr, "data", 4) == 0) {
            data = b.data() + body;
            data_len = len;
            break;
        }
        pos = body + len + (len & 1);
    }
    if (!have_fmt) return unexpected(std::string("wav: missing fmt chunk"));
    if (data == nullptr) return unexpected(std::string("wav: missing data chunk"));
    if (channels < 1 || channels > 64 || rate == 0) return unexpected(std::string("wav: invalid format"));

    const bool is_float = format_tag == 3;
    if (!(format_tag == 1 || is_float))
        return unexpected(std::format("wav: unsupported format tag {}", format_tag));
    if (!((!is_float && (bits == 16 || bits == 24 || bits == 32)) || (is_float && (bits == 32 || bits == 64))))
        return unexpected(std::format("wav: unsupported sample width {} bits", bits));

    const std::size_t bytes_per = static_cast<std::size_t>(bits / 8);
    const std::size_t count = data_len / bytes_per / static_cast<std::size_t>(channels) * static_cast<std::size_t>(channels);

    WavData w;
    w.sample_rate = static_cast<int>(rate);
    w.channels = channels;
    w.samples.resize(count);
    const std::uint8_t* p = data;
    for (std::size_t i = 0; i < count; ++i, p += bytes_per) {
        float v;
        if (is_float && bits == 32) {
            const std::uint32_t u = rd32(p);
            std::memcpy(&v, &u, 4);
        } else if (is_float) {
            double d;
            std::uint64_t u = static_cast<std::uint64_t>(rd32(p)) | static_cast<std::uint64_t>(rd32(p + 4)) << 32;
            std::memcpy(&d, &u, 8);
            v = static_cast<float>(d);
        } else if (bits == 16) {
            v = static_cast<float>(static_cast<std::int16_t>(rd16(p))) / 32768.0f;
        } else if (bits == 24) {
            std::int32_t s = static_cast<std::int32_t>(static_cast<std::uint32_t>(p[0]) << 8 | static_cast<std::uint32_t>(p[1]) << 16 |
                                                       static_cast<std::uint32_t>(p[2]) << 24) >> 8;
            v = static_cast<float>(s) / 8388608.0f;
        } else {
            v = static_cast<float>(static_cast<std::int32_t>(rd32(p)) / 2147483648.0);
        }
        w.samples[i] = v;
    }
    return w;
}

expected<WavData, std::string> read_wav(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return unexpected(std::format("wav: cannot open '{}'", path.string()));
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    auto r = parse_wav(bytes);
    if (!r) return unexpected(std::format("{}: {}", path.string(), r.error()));
    return r;
}

std::vector<std::uint8_t> encode_wav(const WavData& wav, WavFormat format) {
    const int bits = format == WavFormat::Pcm16 ? 16 : format == WavFormat::Pcm24 ? 24 : 32;
    const std::uint32_t bytes_per = static_cast<std::uint32_t>(bits / 8);
    const std::uint32_t data_len = static_cast<std::uint32_t>(wav.samples.size()) * bytes_per;

    std::vector<std::uint8_t> o;
    o.reserve(44 + data_len);
    tag(o, "RIFF");
    wr32(o, 36 + data_len + (data_len & 1));
    tag(o, "WAVE");
    tag(o, "fmt ");
    wr32(o, 16);
    wr16(o, format == WavFormat::Float32 ? 3 : 1);
    wr16(o, static_cast<std::uint16_t>(wav.channels));
    wr32(o, static_cast<std::uint32_t>(wav.sample_rate));
    wr32(o, static_cast<std::uint32_t>(wav.sample_rate) * static_cast<std::uint32_t>(wav.channels) * bytes_per);
    wr16(o, static_cast<std::uint16_t>(static_cast<std::uint32_t>(wav.channels) * bytes_per));
    wr16(o, static_cast<std::uint16_t>(bits));
    tag(o, "data");
    wr32(o, data_len);
    for (const float s : wav.samples) {
        if (format == WavFormat::Float32) {
            std::uint32_t u;
            std::memcpy(&u, &s, 4);
            wr32(o, u);
        } else {
            const double c = std::clamp(static_cast<double>(std::isfinite(s) ? s : 0.0f), -1.0, 1.0);
            if (format == WavFormat::Pcm16) {
                wr16(o, static_cast<std::uint16_t>(static_cast<std::int16_t>(std::lrint(c * 32767.0))));
            } else {
                const auto v = static_cast<std::uint32_t>(static_cast<std::int32_t>(std::lrint(c * 8388607.0)));
                o.push_back(static_cast<std::uint8_t>(v));
                o.push_back(static_cast<std::uint8_t>(v >> 8));
                o.push_back(static_cast<std::uint8_t>(v >> 16));
            }
        }
    }
    if (data_len & 1) o.push_back(0);
    return o;
}

expected<void, std::string> write_wav(const std::filesystem::path& path, const WavData& wav, WavFormat format) {
    const auto bytes = encode_wav(wav, format);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return unexpected(std::format("wav: cannot create '{}'", path.string()));
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!out) return unexpected(std::format("wav: write to '{}' failed", path.string()));
    return {};
}

}  // namespace mimicry
