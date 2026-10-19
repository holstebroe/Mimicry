#include "core/ParamSemantics.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <vector>

namespace mimicry {

namespace {

struct Token {
    std::string_view text;
    ParamGroup group;
    double weight;  // generic words weigh less than specific ones
};

// Spec 04 section 2 [D], plus a few global words [D]. "freq" is a filter word unless the name also says
// lfo, which outweighs it.
constexpr Token kTokens[] = {
    {"osc", ParamGroup::Osc, 1.0},       {"wave", ParamGroup::Osc, 1.0},       {"waveform", ParamGroup::Osc, 1.0},
    {"pitch", ParamGroup::Osc, 1.0},     {"tune", ParamGroup::Osc, 1.0},       {"tuning", ParamGroup::Osc, 1.0},
    {"detune", ParamGroup::Osc, 1.0},    {"unison", ParamGroup::Osc, 1.0},     {"sub", ParamGroup::Osc, 0.8},
    {"noise", ParamGroup::Osc, 0.8},     {"pw", ParamGroup::Osc, 0.8},         {"pwm", ParamGroup::Osc, 0.8},
    {"semi", ParamGroup::Osc, 0.8},      {"octave", ParamGroup::Osc, 0.8},     {"fine", ParamGroup::Osc, 0.6},
    {"cutoff", ParamGroup::Filter, 1.2}, {"freq", ParamGroup::Filter, 0.7},    {"frequency", ParamGroup::Filter, 0.7},
    {"res", ParamGroup::Filter, 1.0},    {"reso", ParamGroup::Filter, 1.0},    {"resonance", ParamGroup::Filter, 1.0},
    {"q", ParamGroup::Filter, 0.8},      {"drive", ParamGroup::Filter, 0.7},   {"lpf", ParamGroup::Filter, 1.0},
    {"hpf", ParamGroup::Filter, 1.0},    {"filter", ParamGroup::Filter, 1.0},  {"vcf", ParamGroup::Filter, 1.0},
    {"attack", ParamGroup::Env, 1.0},    {"decay", ParamGroup::Env, 1.0},      {"sustain", ParamGroup::Env, 1.0},
    {"release", ParamGroup::Env, 1.0},   {"adsr", ParamGroup::Env, 1.2},       {"env", ParamGroup::Env, 1.0},
    {"envelope", ParamGroup::Env, 1.0},  {"amt", ParamGroup::Env, 0.3},        {"eg", ParamGroup::Env, 1.0},
    {"lfo", ParamGroup::Lfo, 1.5},       {"rate", ParamGroup::Lfo, 0.6},       {"depth", ParamGroup::Lfo, 0.6},
    {"mod", ParamGroup::Lfo, 0.5},       {"vibrato", ParamGroup::Lfo, 1.0},    {"tremolo", ParamGroup::Lfo, 1.0},
    {"reverb", ParamGroup::Fx, 1.5},     {"delay", ParamGroup::Fx, 1.2},       {"chorus", ParamGroup::Fx, 1.5},
    {"mix", ParamGroup::Fx, 0.6},        {"wet", ParamGroup::Fx, 1.0},         {"dry", ParamGroup::Fx, 0.8},
    {"feedback", ParamGroup::Fx, 0.9},   {"size", ParamGroup::Fx, 0.7},        {"fx", ParamGroup::Fx, 1.2},
    {"phaser", ParamGroup::Fx, 1.5},     {"flanger", ParamGroup::Fx, 1.5},     {"room", ParamGroup::Fx, 1.0},
    {"volume", ParamGroup::Amp, 1.0},    {"gain", ParamGroup::Amp, 0.8},       {"level", ParamGroup::Amp, 0.8},
    {"pan", ParamGroup::Amp, 1.0},       {"amp", ParamGroup::Amp, 1.0},        {"vca", ParamGroup::Amp, 1.0},
    {"output", ParamGroup::Amp, 0.8},    {"vol", ParamGroup::Amp, 1.0},
    {"master", ParamGroup::Global, 0.8}, {"global", ParamGroup::Global, 0.8},  {"glide", ParamGroup::Global, 1.0},
    {"portamento", ParamGroup::Global, 1.0}, {"poly", ParamGroup::Global, 0.8}, {"voices", ParamGroup::Global, 0.8},
    {"bend", ParamGroup::Global, 0.8},
};

std::vector<std::string> tokens(std::string_view text) {
    std::vector<std::string> out;
    std::string cur;
    auto flush = [&] {
        if (!cur.empty()) out.push_back(cur);
        cur.clear();
    };
    for (std::size_t i = 0; i < text.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (std::isalpha(c)) {
            // camelCase boundary: "FilterCutoff" -> filter, cutoff
            if (std::isupper(c) && !cur.empty() && i > 0 && std::islower(static_cast<unsigned char>(text[i - 1]))) flush();
            cur.push_back(static_cast<char>(std::tolower(c)));
        } else {
            flush();  // digits, spaces, punctuation separate tokens ("Osc1" -> osc)
        }
    }
    flush();
    return out;
}

void score(std::string_view text, double scale, std::array<double, kParamGroupCount>& s) {
    for (const auto& t : tokens(text))
        for (const auto& k : kTokens)
            if (t == k.text) s[std::size_t(k.group)] += scale * k.weight;
}

bool has_token(std::string_view text, std::initializer_list<std::string_view> words) {
    for (const auto& t : tokens(text))
        for (auto w : words)
            if (t == w) return true;
    return false;
}

}  // namespace

std::string_view to_string(ParamGroup group) noexcept {
    switch (group) {
        case ParamGroup::Osc: return "osc";
        case ParamGroup::Filter: return "filter";
        case ParamGroup::Env: return "env";
        case ParamGroup::Lfo: return "lfo";
        case ParamGroup::Fx: return "fx";
        case ParamGroup::Amp: return "amp";
        case ParamGroup::Global: return "global";
        case ParamGroup::Unknown: break;
    }
    return "unknown";
}

ParamGroup parse_group(std::string_view text) noexcept {
    for (int g = 0; g < kParamGroupCount; ++g)
        if (to_string(ParamGroup(g)) == text) return ParamGroup(g);
    return ParamGroup::Unknown;
}

Semantics classify_param(std::string_view name, std::string_view module, std::string_view units) {
    std::array<double, kParamGroupCount> s{};
    score(name, 1.0, s);
    score(module, 0.6, s);
    double total = 0, best = 0;
    int arg = int(ParamGroup::Unknown);
    for (int g = 0; g < kParamGroupCount; ++g) {
        total += s[std::size_t(g)];
        if (s[std::size_t(g)] > best) {
            best = s[std::size_t(g)];
            arg = g;
        }
    }
    if (best <= 0) {
        if (units == "Hz" || units == "kHz") return {ParamGroup::Filter, 0.2};
        if (units == "ms" || units == "s") return {ParamGroup::Env, 0.2};
        if (units == "dB") return {ParamGroup::Amp, 0.2};
        return {};
    }
    // Strength of the winning evidence times how much it dominates the rest.
    const double strength = std::min(1.0, best / 1.2);
    return {ParamGroup(arg), std::clamp(0.3 + 0.6 * strength * (best / total), 0.0, 0.95)};
}

bool is_output_level(std::string_view name, std::string_view module) {
    // A level inside an oscillator, envelope or effect module is a mix control, not the output.
    if (has_token(module, {"osc", "env", "lfo", "fx", "filter", "reverb", "delay", "chorus"})) return false;
    if (has_token(name, {"osc", "sub", "noise", "env", "lfo", "send", "mix"})) return false;
    return has_token(name, {"volume", "gain", "level", "output", "vol"});
}

bool is_pitch_param(std::string_view name) {
    if (has_token(name, {"lfo", "env", "mod", "amt", "amount", "depth", "bend"})) return false;
    return has_token(name, {"tune", "tuning", "fine", "pitch"});
}

bool is_seed_param(std::string_view name) { return has_token(name, {"seed", "random", "rnd"}); }

}  // namespace mimicry
