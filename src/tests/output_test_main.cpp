// Output files (docs/spec/07_OUTPUT_VALIDATION.md): the `.vstpreset` layout and its round trip with the state
// blob (T14, the host-side half is in cli_*), and the diff.ppm image.

#include <cmath>
#include <cstdio>
#include <cstring>
#include <numbers>
#include <string>

#include "output/Ppm.hpp"
#include "output/VstPreset.hpp"

namespace {

using namespace mimicry;

int failures = 0;

void check(bool ok, const std::string& what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
        ++failures;
    }
}

std::vector<std::uint8_t> blob(const std::vector<std::uint8_t>& comp, const std::vector<std::uint8_t>& ctrl) {
    return output::vst3_state_from_chunks({comp, ctrl});
}

void test_vstpreset() {
    const std::string cls = "ABCDEF0123456789ABCDEF0123456789";
    const std::vector<std::uint8_t> comp{1, 2, 3, 4, 5}, ctrl{9, 8, 7};
    const auto state = blob(comp, ctrl);
    const auto split = output::split_vst3_state(state);
    check(split && split->component == comp && split->controller == ctrl, "state blob splits into its two parts");
    check(!output::split_vst3_state({1, 0, 0}) && !output::split_vst3_state({9, 0, 0, 0, 1}), "a malformed blob is rejected");

    auto bytes = output::vstpreset_from_state(cls, state);
    check(bytes.has_value(), "vstpreset: written");
    if (!bytes) return;
    check(std::memcmp(bytes->data(), "VST3", 4) == 0 && (*bytes)[4] == 1, "vstpreset: header and version");
    check(std::memcmp(bytes->data() + 8, cls.data(), 32) == 0, "vstpreset: class id");
    std::uint64_t list = 0;
    for (int i = 7; i >= 0; --i) list = (list << 8) | (*bytes)[40 + std::size_t(i)];
    check(list + 8 + 2 * 20 == bytes->size() && std::memcmp(bytes->data() + list, "List", 4) == 0, "vstpreset: the chunk list closes the file");
    const auto back = output::parse_vstpreset(*bytes);
    check(back && back->class_id == cls && back->chunks.component == comp && back->chunks.controller == ctrl, "vstpreset: parse round trip");
    check(back && output::vst3_state_from_chunks(back->chunks) == state, "vstpreset: back to the same state blob");

    auto lower = output::vstpreset_from_state("abcdef0123456789abcdef0123456789", state);
    check(lower && std::memcmp(lower->data() + 8, cls.data(), 32) == 0, "vstpreset: the class id is written upper case");
    auto no_ctrl = output::vstpreset_bytes(cls, {comp, {}});
    const auto nb = no_ctrl ? output::parse_vstpreset(*no_ctrl) : mimicry::unexpected(std::string("x"));
    check(nb && nb->chunks.controller.empty() && nb->chunks.component == comp, "vstpreset: no controller chunk when the controller has no state");
    check(!output::vstpreset_bytes("short", {comp, ctrl}) && !output::vstpreset_bytes(std::string(32, 'z'), {comp, ctrl}),
          "vstpreset: a class id must be 32 hex characters");
    auto broken = *bytes;
    broken.resize(broken.size() - 5);
    check(!output::parse_vstpreset(broken) && !output::parse_vstpreset({}), "vstpreset: truncated files are rejected");
}

void test_ppm() {
    const int sr = 48000;
    std::vector<float> a(sr), b(sr);
    for (int i = 0; i < sr; ++i) {
        a[std::size_t(i)] = 0.5f * float(std::sin(2.0 * std::numbers::pi * 440.0 * i / sr));
        b[std::size_t(i)] = 0.5f * float(std::sin(2.0 * std::numbers::pi * 1760.0 * i / sr));
    }
    const auto same = output::spectrogram_comparison(a, a, sr, 0, 60, 40);
    const auto diff = output::spectrogram_comparison(a, b, sr, 0, 60, 40);
    check(same.width == 60 && same.height == 3 * 40 + 4 && same.rgb.size() == std::size_t(same.width) * std::size_t(same.height) * 3, "ppm: size");
    const auto bytes = output::ppm_bytes(same);
    const std::string head(bytes.begin(), bytes.begin() + 14);
    check(head.rfind("P6\n60 124\n255\n", 0) == 0 && bytes.size() == 14 + same.rgb.size(), "ppm: P6 header and payload");
    const auto bmp = output::bmp_bytes(same);
    const std::size_t row = (60 * 3 + 3) / 4 * 4;
    check(bmp.size() == 54 + row * 124 && bmp[0] == 'B' && bmp[1] == 'M' && bmp[18] == 60 && bmp[22] == 124 && bmp[28] == 24, "bmp: header, size and padded rows");
    // The first stored row is the bottom one, in B, G, R order.
    const auto* top = same.rgb.data();
    const auto* bottom = same.rgb.data() + std::size_t(same.height - 1) * 60 * 3;
    check(bmp[54] == bottom[2] && bmp[55] == bottom[1] && bmp[56] == bottom[0], "bmp: bottom row first, BGR");
    check(bmp[54 + row * 123] == top[2], "bmp: top row last");
    // The difference panel is white where the match equals the target and coloured where it does not.
    auto panel_energy = [](const output::Image& im) {
        double e = 0;
        for (int y = 2 * 42; y < 2 * 42 + 40; ++y)
            for (int x = 0; x < im.width; ++x) {
                const auto* px = im.rgb.data() + (std::size_t(y) * std::size_t(im.width) + std::size_t(x)) * 3;
                e += (255.0 - px[0]) + (255.0 - px[1]) + (255.0 - px[2]);
            }
        return e;
    };
    check(panel_energy(same) < 1.0, "ppm: identical signals give a white difference panel");
    check(panel_energy(diff) > 1e4, "ppm: different signals give a coloured difference panel");
    // The match is compared at the solved gain: +6 dB on half the amplitude equals the target.
    std::vector<float> half(a);
    for (auto& v : half) v *= 0.5f;
    check(panel_energy(output::spectrogram_comparison(a, half, sr, 6.0206, 60, 40)) < 2e3, "ppm: the match is scaled by the gain first");
}

}  // namespace

int main() {
    test_vstpreset();
    test_ppm();
    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::puts("mimicry_output_test: all passed");
    return 0;
}
