#include "output/Ppm.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>
#include <string>

#include "core/Fft.hpp"

namespace mimicry::output {

namespace {

constexpr std::size_t kFft = 2048;
constexpr double kFmin = 40.0;

// dB grid [column][row]; rows from the highest frequency down.
struct Grid {
    int cols = 0, rows = 0;
    std::vector<float> db;
    [[nodiscard]] float& at(int c, int r) { return db[std::size_t(c) * std::size_t(rows) + std::size_t(r)]; }
    [[nodiscard]] float at(int c, int r) const { return db[std::size_t(c) * std::size_t(rows) + std::size_t(r)]; }
};

Grid spectrogram(const std::vector<float>& x, int sr, int cols, int rows, std::size_t length, double gain_db) {
    Grid g;
    g.cols = cols;
    g.rows = rows;
    g.db.assign(std::size_t(cols) * std::size_t(rows), -120.0f);
    if (x.empty() || length == 0) return g;
    const Fft fft(kFft);
    std::vector<double> window(kFft);
    for (std::size_t i = 0; i < kFft; ++i) window[i] = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * double(i) / double(kFft));
    const double gain = std::pow(10.0, gain_db / 20.0);
    const double fmax = 0.5 * sr;
    std::vector<std::complex<double>> buf(kFft);
    for (int c = 0; c < cols; ++c) {
        const long centre = long((double(c) + 0.5) * double(length) / double(cols));
        for (std::size_t i = 0; i < kFft; ++i) {
            const long at = centre - long(kFft / 2) + long(i);
            const double v = (at >= 0 && at < long(x.size())) ? double(x[std::size_t(at)]) * gain : 0.0;
            buf[i] = v * window[i];
        }
        fft.forward(buf);
        for (int r = 0; r < rows; ++r) {
            // Row 0 is fmax; each row averages the bins between its edges (at least one bin).
            const double f1 = kFmin * std::pow(fmax / kFmin, 1.0 - double(r) / double(rows));
            const double f0 = kFmin * std::pow(fmax / kFmin, 1.0 - double(r + 1) / double(rows));
            const auto b0 = std::size_t(std::clamp(f0 / sr * double(kFft), 0.0, double(kFft / 2)));
            const auto b1 = std::max(b0 + 1, std::size_t(std::clamp(f1 / sr * double(kFft), 0.0, double(kFft / 2))));
            double p = 0;
            for (std::size_t b = b0; b < std::min(b1, kFft / 2 + 1); ++b) p = std::max(p, std::norm(buf[b]));
            g.at(c, r) = float(10.0 * std::log10(std::max(p / (double(kFft) * double(kFft) / 16.0), 1e-12)));
        }
    }
    return g;
}

void heat(float t, std::uint8_t* px) {  // 0..1: black, purple, orange, yellow
    t = std::clamp(t, 0.0f, 1.0f);
    const float r = std::clamp(t * 2.0f, 0.0f, 1.0f), g = std::clamp(t * 2.0f - 0.8f, 0.0f, 1.0f), b = std::clamp(0.6f - std::abs(t - 0.3f) * 2.0f, 0.0f, 1.0f);
    px[0] = std::uint8_t(255 * r);
    px[1] = std::uint8_t(255 * g);
    px[2] = std::uint8_t(255 * b);
}

void diverge(float d, float range, std::uint8_t* px) {  // blue (quieter) .. white .. red (louder)
    const float t = std::clamp(d / range, -1.0f, 1.0f);
    const float a = std::abs(t);
    if (t >= 0) {
        px[0] = 255;
        px[1] = px[2] = std::uint8_t(255 * (1 - a));
    } else {
        px[2] = 255;
        px[0] = px[1] = std::uint8_t(255 * (1 - a));
    }
}

}  // namespace

Image spectrogram_comparison(const std::vector<float>& target, const std::vector<float>& match, int sample_rate, double match_gain_db,
                             int columns, int rows) {
    const std::size_t length = std::max(target.size(), match.size());
    const Grid t = spectrogram(target, sample_rate, columns, rows, length, 0);
    const Grid m = spectrogram(match, sample_rate, columns, rows, length, match_gain_db);
    float peak = -120.0f;
    for (float v : t.db) peak = std::max(peak, v);
    const float range = 80.0f;
    const int gap = 2;
    Image img;
    img.width = columns;
    img.height = 3 * rows + 2 * gap;
    img.rgb.assign(std::size_t(img.width) * std::size_t(img.height) * 3, 40);
    auto px = [&](int c, int y) { return img.rgb.data() + (std::size_t(y) * std::size_t(img.width) + std::size_t(c)) * 3; };
    for (int c = 0; c < columns; ++c)
        for (int r = 0; r < rows; ++r) {
            heat((t.at(c, r) - (peak - range)) / range, px(c, r));
            heat((m.at(c, r) - (peak - range)) / range, px(c, rows + gap + r));
            // Below the target's noise floor the difference is noise, not information: faded out.
            const float d = m.at(c, r) - t.at(c, r);
            const float live = std::clamp((std::max(t.at(c, r), m.at(c, r)) - (peak - range)) / 20.0f, 0.0f, 1.0f);
            diverge(d * live, 24.0f, px(c, 2 * (rows + gap) + r));
        }
    return img;
}

std::vector<std::uint8_t> ppm_bytes(const Image& image) {
    const std::string head = "P6\n" + std::to_string(image.width) + " " + std::to_string(image.height) + "\n255\n";
    std::vector<std::uint8_t> out(head.begin(), head.end());
    out.insert(out.end(), image.rgb.begin(), image.rgb.end());
    return out;
}

}  // namespace mimicry::output

namespace mimicry::output {

std::vector<std::uint8_t> bmp_bytes(const Image& image) {
    const std::size_t row = (std::size_t(image.width) * 3 + 3) / 4 * 4;
    const std::size_t pixels = row * std::size_t(image.height);
    std::vector<std::uint8_t> out;
    out.reserve(54 + pixels);
    auto put = [&](std::uint64_t v, int bytes) {
        for (int i = 0; i < bytes; ++i) out.push_back(std::uint8_t(v >> (8 * i)));
    };
    out.push_back('B');
    out.push_back('M');
    put(54 + pixels, 4);
    put(0, 4);
    put(54, 4);       // pixel data offset
    put(40, 4);       // BITMAPINFOHEADER
    put(std::uint32_t(image.width), 4);
    put(std::uint32_t(image.height), 4);  // positive: rows run bottom to top
    put(1, 2);
    put(24, 2);
    put(0, 4);        // BI_RGB
    put(pixels, 4);
    put(2835, 4);
    put(2835, 4);
    put(0, 4);
    put(0, 4);
    for (int y = image.height - 1; y >= 0; --y) {
        const auto* src = image.rgb.data() + std::size_t(y) * std::size_t(image.width) * 3;
        for (int x = 0; x < image.width; ++x) {
            out.push_back(src[x * 3 + 2]);  // B, G, R
            out.push_back(src[x * 3 + 1]);
            out.push_back(src[x * 3]);
        }
        out.insert(out.end(), row - std::size_t(image.width) * 3, 0);
    }
    return out;
}

}  // namespace mimicry::output
