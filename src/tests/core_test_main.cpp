// Unit tests for the core utilities: Rng, Json, Wav, Fft (WP0.3).

#include <cmath>
#include <complex>
#include <cstdio>
#include <vector>

#include "core/Fft.hpp"
#include "core/Json.hpp"
#include "core/Rng.hpp"
#include "core/Wav.hpp"

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

void test_rng() {
    mimicry::Rng a(42), b(42), c(43);
    bool same = true, differs = false;
    for (int i = 0; i < 100; ++i) {
        const auto x = a.next_u64();
        same = same && x == b.next_u64();
        differs = differs || x != c.next_u64();
    }
    check(same, "Rng: same seed, same sequence");
    check(differs, "Rng: different seed, different sequence");

    mimicry::Rng r(7);
    double sum = 0, sum2 = 0;
    bool in_range = true;
    const int n = 200000;
    for (int i = 0; i < n; ++i) {
        const double u = r.uniform();
        in_range = in_range && u >= 0.0 && u < 1.0;
        sum += u;
    }
    check(in_range, "Rng: uniform in [0,1)");
    check(std::fabs(sum / n - 0.5) < 0.01, "Rng: uniform mean");
    sum = 0;
    for (int i = 0; i < n; ++i) {
        const double g = r.normal();
        sum += g;
        sum2 += g * g;
    }
    check(std::fabs(sum / n) < 0.02 && std::fabs(sum2 / n - 1.0) < 0.03, "Rng: normal mean and variance");
}

void test_json() {
    using namespace mimicry::json;
    auto v = parse(R"({"a": [1, 2.5, -3e2, true, false, null], "s": "x\"\n\u00e9\ud83d\ude00", "o": {"k": 1}})");
    check(v.has_value(), "Json: parses");
    if (!v) return;
    check((*v)["a"].as_array().size() == 6, "Json: array size");
    check((*v)["a"].as_array()[2].as_number() == -300.0, "Json: exponent");
    check((*v)["a"].as_array()[3].as_bool(), "Json: true");
    check((*v)["a"].as_array()[5].is_null(), "Json: null");
    check((*v)["s"].as_string() == "x\"\n\xC3\xA9\xF0\x9F\x98\x80", "Json: string escapes and utf-8");
    check((*v)["o"]["k"].as_number() == 1.0, "Json: nested lookup");
    check((*v)["missing"].is_null() && (*v)["a"]["x"].is_null(), "Json: missing keys are null");

    auto round = parse(v->dump());
    check(round && *round == *v, "Json: compact round trip");
    auto pretty = parse(v->dump(2));
    check(pretty && *pretty == *v, "Json: pretty round trip");

    Value built;
    built.set("id", 7).set("name", "x").set("list", Array{1, 2});
    check(built.dump() == R"({"id":7,"list":[1,2],"name":"x"})", "Json: writer output");
    check(Value(0.1).dump() == "0.1", "Json: shortest double");
    check(Value(std::nan("")).dump() == "null", "Json: NaN writes null");

    for (const char* bad : {"", "{", "[1,]", "{\"a\":}", "tru", "\"abc", "[1] x", "{\"a\" 1}", "01x", "\"\\q\""})
        check(!parse(bad).has_value(), bad);
    std::string deep(500, '[');
    check(!parse(deep).has_value(), "Json: depth limit");
}

void test_wav() {
    mimicry::WavData w;
    w.sample_rate = 44100;
    w.channels = 2;
    for (int i = 0; i < 200; ++i) w.samples.push_back(std::sin(static_cast<float>(i) * 0.1f) * 0.8f);

    for (auto fmt : {mimicry::WavFormat::Float32, mimicry::WavFormat::Pcm16, mimicry::WavFormat::Pcm24}) {
        const auto bytes = mimicry::encode_wav(w, fmt);
        const auto r = mimicry::parse_wav(bytes);
        check(r.has_value(), "Wav: re-parses");
        if (!r) continue;
        check(r->sample_rate == 44100 && r->channels == 2 && r->samples.size() == w.samples.size(), "Wav: header fields");
        const float tol = fmt == mimicry::WavFormat::Float32 ? 0.0f : fmt == mimicry::WavFormat::Pcm16 ? 1e-4f : 1e-6f;
        float err = 0;
        for (std::size_t i = 0; i < w.samples.size(); ++i) err = std::fmax(err, std::fabs(r->samples[i] - w.samples[i]));
        check(err <= tol, "Wav: round trip error within quantisation");
    }
    check(!mimicry::parse_wav({0, 1, 2}).has_value(), "Wav: rejects garbage");
    auto cut = mimicry::encode_wav(w);
    cut.resize(60);
    check(mimicry::parse_wav(cut).has_value(), "Wav: tolerates a truncated data chunk");
    check(!mimicry::read_wav("/nonexistent/x.wav").has_value(), "Wav: missing file is an error");
}

void test_fft() {
    using C = std::complex<double>;
    const std::size_t n = 64;
    mimicry::Fft fft(n);
    std::vector<C> x(n);
    mimicry::Rng rng(3);
    for (auto& v : x) v = {rng.normal(), rng.normal()};
    auto y = x;
    fft.forward(y);
    // Against a direct DFT.
    double worst = 0;
    for (std::size_t k = 0; k < n; ++k) {
        C acc = 0;
        for (std::size_t t = 0; t < n; ++t)
            acc += x[t] * std::polar(1.0, -2.0 * M_PI * static_cast<double>(k * t) / static_cast<double>(n));
        worst = std::fmax(worst, std::abs(acc - y[k]));
    }
    check(worst < 1e-9, "Fft: matches the DFT");
    fft.inverse(y);
    worst = 0;
    for (std::size_t i = 0; i < n; ++i) worst = std::fmax(worst, std::abs(y[i] - x[i]));
    check(worst < 1e-12, "Fft: inverse recovers the input");

    std::vector<float> sine(n);
    for (std::size_t i = 0; i < n; ++i) sine[i] = static_cast<float>(std::sin(2.0 * M_PI * 8.0 * static_cast<double>(i) / n));
    const auto mag = fft.magnitude(sine.data());
    check(mag.size() == n / 2 + 1 && std::fabs(mag[8] - n / 2.0) < 1e-4 && mag[3] < 1e-4, "Fft: sine peaks at its bin");
    check(!mimicry::Fft::is_power_of_two(48) && mimicry::Fft::is_power_of_two(1024), "Fft: power-of-two check");
}

}  // namespace

int main() {
    test_rng();
    test_json();
    test_wav();
    test_fft();
    if (failures == 0) std::puts("mimicry_core_test: all checks passed");
    return failures == 0 ? 0 : 1;
}
