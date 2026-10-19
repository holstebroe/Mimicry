#include "features/Pitch.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

#include "core/Fft.hpp"
#include "features/Spectral.hpp"

namespace mimicry::features {

namespace {

struct Peak {
    double freq, amp;
};

}  // namespace

PitchEstimate estimate_f0(const std::vector<float>& mono, int rate, double fmin, double fmax) {
    PitchEstimate result;
    if (mono.size() < 256 || rate <= 0) return result;

    // The loudest stretch of up to 64k samples.
    const std::size_t seg = std::min<std::size_t>(mono.size(), 65536);
    std::size_t best_start = 0;
    double best_energy = -1;
    const std::size_t block = 2048;
    for (std::size_t s = 0; s + seg <= mono.size() || s == 0; s += block) {
        double e = 0;
        for (std::size_t i = s; i < std::min(mono.size(), s + seg); ++i) e += double(mono[i]) * mono[i];
        if (e > best_energy) {
            best_energy = e;
            best_start = s;
        }
        if (s + seg >= mono.size()) break;
    }
    if (best_energy <= 1e-12) return result;

    const std::size_t n = std::max<std::size_t>(8192, next_pow2(seg * 2));
    std::vector<float> buf(n, 0.0f);
    double wsum = 0;
    for (std::size_t i = 0; i < seg; ++i) {
        const double w = 0.5 - 0.5 * std::cos(2.0 * 3.141592653589793 * (double(i) + 0.5) / double(seg));
        const std::size_t idx = best_start + i;
        buf[i] = idx < mono.size() ? static_cast<float>(mono[idx] * w) : 0.0f;
        wsum += w;
    }
    Fft fft(n);
    const auto mag = fft.magnitude(buf.data());
    const double bin_hz = double(rate) / double(n);

    // Peaks: local maxima within 60 dB of the strongest, frequencies from log-parabolic interpolation.
    double max_mag = 0;
    for (std::size_t k = 1; k + 1 < mag.size(); ++k) max_mag = std::max(max_mag, mag[k]);
    if (max_mag <= 0) return result;
    std::vector<Peak> peaks;
    const std::size_t kmin = std::max<std::size_t>(2, std::size_t(fmin * 0.5 / bin_hz));
    for (std::size_t k = kmin; k + 1 < mag.size(); ++k) {
        if (mag[k] > mag[k - 1] && mag[k] >= mag[k + 1] && mag[k] > max_mag * 1e-3) {
            const double a = std::log(mag[k - 1] + 1e-30), b = std::log(mag[k] + 1e-30), c = std::log(mag[k + 1] + 1e-30);
            const double denom = a - 2 * b + c;
            const double d = denom != 0 ? 0.5 * (a - c) / denom : 0.0;
            peaks.push_back({(double(k) + d) * bin_hz, mag[k] / (wsum * 0.5)});
        }
    }
    if (peaks.empty()) return result;
    std::sort(peaks.begin(), peaks.end(), [](const Peak& x, const Peak& y) { return x.amp > y.amp; });
    if (peaks.size() > 64) peaks.resize(64);
    const double amp_ref = peaks.front().amp * 1e-3;

    const double nyq = 0.5 * rate;
    auto evaluate = [&](double f0, double& refined) {
        double score = 0;
        for (int iter = 0; iter < 3; ++iter) {
            const int h_max = std::min(16, int(0.95 * nyq / f0));
            double num = 0, den = 0;
            score = 0;
            for (int k = 1; k <= h_max; ++k) {
                const double target = k * f0, tol = std::min(0.03 * target, 0.35 * f0);
                const Peak* hit = nullptr;
                for (const auto& p : peaks)
                    if (std::fabs(p.freq - target) <= tol && (!hit || p.amp > hit->amp)) hit = &p;
                if (hit) {
                    score += std::log(1.0 + hit->amp / amp_ref);
                    num += hit->amp * k * hit->freq;
                    den += hit->amp * k * k;
                } else {
                    score -= 1.0;  // a missing harmonic costs, which rules out sub-octave combs [D]
                }
            }
            if (den <= 0) return -1e9;
            f0 = num / den;
        }
        refined = f0;
        return score;
    };

    double best_score = -1e9, best_f0 = 0;
    const std::size_t top = std::min<std::size_t>(peaks.size(), 12);
    for (std::size_t i = 0; i < top; ++i) {
        for (int m = 1; m <= 8; ++m) {
            const double cand = peaks[i].freq / m;
            if (cand < fmin || cand > fmax) continue;
            double refined = 0;
            const double s = evaluate(cand, refined);
            if (s > best_score + 1e-9 || (std::fabs(s - best_score) <= 1e-9 && refined > best_f0)) {
                best_score = s;
                best_f0 = refined;
            }
        }
    }
    if (best_f0 <= 0) return result;

    // Confidence: the comb covers half the spectrum, so noise scores 0.5 and a clean tone 1.
    double total = 0, on_comb = 0;
    const double band = best_f0 * 0.25;
    for (std::size_t k = 1; k < mag.size(); ++k) {
        const double f = double(k) * bin_hz;
        if (f < 20 || f > 0.95 * nyq) continue;
        const double p = mag[k] * mag[k];
        total += p;
        const double r = f / best_f0;
        const double nearest = std::max(1.0, std::round(r));
        if (std::fabs(f - nearest * best_f0) <= band) on_comb += p;
    }
    result.f0 = best_f0;
    result.confidence = total > 0 ? std::clamp((on_comb / total - 0.5) * 2.0, 0.0, 1.0) : 0.0;
    return result;
}

NotePitch note_from_hz(double hz) {
    NotePitch np;
    if (!(hz > 0)) return np;
    const double midi = 69.0 + 12.0 * std::log2(hz / 440.0);
    np.key = static_cast<int>(std::lround(midi));
    np.cents = (midi - np.key) * 100.0;
    return np;
}

double hz_from_note(int key, double cents) { return 440.0 * std::pow(2.0, (key - 69.0) / 12.0 + cents / 1200.0); }

std::optional<int> parse_note_name(std::string_view text) {
    if (text.empty()) return std::nullopt;
    if (std::isdigit(static_cast<unsigned char>(text[0])) || text[0] == '-') {
        try {
            const int v = std::stoi(std::string(text));
            return v >= 0 && v <= 127 ? std::optional<int>(v) : std::nullopt;
        } catch (...) {
            return std::nullopt;
        }
    }
    static const int semis[7] = {9, 11, 0, 2, 4, 5, 7};  // A B C D E F G
    const char letter = static_cast<char>(std::toupper(static_cast<unsigned char>(text[0])));
    if (letter < 'A' || letter > 'G') return std::nullopt;
    int semitone = semis[letter - 'A'];
    std::size_t i = 1;
    if (i < text.size() && (text[i] == '#' || text[i] == 'b')) semitone += text[i++] == '#' ? 1 : -1;
    if (i >= text.size()) return std::nullopt;
    try {
        std::size_t used = 0;
        const int octave = std::stoi(std::string(text.substr(i)), &used);
        if (i + used != text.size()) return std::nullopt;
        const int key = (octave + 1) * 12 + semitone;
        return key >= 0 && key <= 127 ? std::optional<int>(key) : std::nullopt;
    } catch (...) {
        return std::nullopt;
    }
}

}  // namespace mimicry::features
