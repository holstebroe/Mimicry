#pragma once
// DSP and parameter table of mimicry_test_synth (docs/spec/07_OUTPUT_VALIDATION.md section 5).
//
// A deliberately small synth with planted hazards, independent of any plugin format so the CLAP module and
// the clap-wrapper VST3 build run the same code. The hazards are used by the resilience tests (T6-T9, T16):
//   haz_nan    > 0.90          output turns NaN
//   haz_a      > 0.80 && haz_b > 0.90   null dereference (the T8 constraint)
//   haz_hang   > 0.95          process() never returns
//   haz_poison > 0.90          filter state turns NaN and stays NaN until the instance is re-activated
//   lfo_seed   == 0            random LFO phase per note (repeatability floor); > 0 is deterministic
//   program    changes        overwrites wave and cutoff (the program-change parameter, pinned by hosts)
//   restart    crosses 0.5    asks the host for a restart / parameter rescan (T16)
//   haz_sticky > 0.90          output turns NaN for the life of the instance: survives reset, re-activation
//                              and state load (the canary of spec 05 section 5 must restart the worker)
//   haz_throw  > 0.90          process() throws a C++ exception (spec 05 section 9: caught, HostError)
//   haz_rename crosses 0.5     renames "Hidden Drive" and asks for a parameter rescan: the parameter table
//                              changes between candidates (spec 05 section 9). Hidden, so never searched

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <random>
#include <stdexcept>
#include <thread>

namespace mimicry::testsynth {

enum ParamId : std::uint32_t {
    kWave = 0,
    kCutoff,
    kAttack,
    kDecay,
    kSustain,
    kRelease,
    kLevel,
    kDetune,
    kLfoRate,
    kLfoDepth,
    kLfoSeed,
    kHazNan,
    kHazA,
    kHazB,
    kHazHang,
    kHazPoison,
    kProgram,
    kRestart,
    kBypass,
    kMeter,   // read-only
    kHidden,  // hidden
    kHazSticky,
    kHazThrow,
    kHazRename,  // hidden
    kParamCount
};

enum class Scale { Linear, Log };

struct ParamInfo {
    ParamId id;
    const char* name;
    const char* module;
    const char* unit;  // "" if none
    double min, max, def;
    bool stepped;
    bool hidden;
    bool readonly;
    bool bypass;
    Scale scale;
};

inline constexpr ParamInfo kParams[kParamCount] = {
    {kWave, "Wave", "Osc", "", 0, 1, 0, true, false, false, false, Scale::Linear},
    {kCutoff, "Cutoff", "Filter", "Hz", 20, 20000, 8000, false, false, false, false, Scale::Log},
    {kAttack, "Attack", "Env", "ms", 1, 2000, 5, false, false, false, false, Scale::Log},
    {kDecay, "Decay", "Env", "ms", 1, 2000, 200, false, false, false, false, Scale::Log},
    {kSustain, "Sustain", "Env", "%", 0, 100, 70, false, false, false, false, Scale::Linear},
    {kRelease, "Release", "Env", "ms", 1, 5000, 150, false, false, false, false, Scale::Log},
    {kLevel, "Level", "Amp", "dB", -48, 6, -12, false, false, false, false, Scale::Linear},
    {kDetune, "Detune", "Osc", "cents", -100, 100, 0, false, false, false, false, Scale::Linear},
    {kLfoRate, "LFO Rate", "LFO", "Hz", 0.1, 20, 5, false, false, false, false, Scale::Log},
    {kLfoDepth, "LFO Depth", "LFO", "cents", 0, 100, 0, false, false, false, false, Scale::Linear},
    {kLfoSeed, "LFO Seed", "LFO", "", 0, 255, 1, true, false, false, false, Scale::Linear},
    {kHazNan, "Hazard NaN", "Hazards", "", 0, 1, 0, false, false, false, false, Scale::Linear},
    {kHazA, "Hazard A", "Hazards", "", 0, 1, 0, false, false, false, false, Scale::Linear},
    {kHazB, "Hazard B", "Hazards", "", 0, 1, 0, false, false, false, false, Scale::Linear},
    {kHazHang, "Hazard Hang", "Hazards", "", 0, 1, 0, false, false, false, false, Scale::Linear},
    {kHazPoison, "Hazard Poison", "Hazards", "", 0, 1, 0, false, false, false, false, Scale::Linear},
    {kProgram, "Program", "Global", "", 0, 3, 0, true, false, false, false, Scale::Linear},
    {kRestart, "Restart", "Global", "", 0, 1, 0, false, false, false, false, Scale::Linear},
    {kBypass, "Bypass", "Global", "", 0, 1, 0, true, false, false, true, Scale::Linear},
    {kMeter, "Meter", "Global", "dB", -96, 0, -96, false, false, true, false, Scale::Linear},
    {kHidden, "Hidden Drive", "Global", "", 0, 1, 0, false, true, false, false, Scale::Linear},
    {kHazSticky, "Hazard Sticky", "Hazards", "", 0, 1, 0, false, false, false, false, Scale::Linear},
    {kHazThrow, "Hazard Throw", "Hazards", "", 0, 1, 0, false, false, false, false, Scale::Linear},
    {kHazRename, "Hazard Rename", "Hazards", "", 0, 1, 0, false, true, false, false, Scale::Linear},
};

inline constexpr const char* kRenamedHidden = "Hidden Drive (renamed)";

// Programs overwrite the wave and cutoff parameters (the reason hosts pin the program parameter).
struct Program {
    double wave, cutoff;
};
inline constexpr Program kPrograms[4] = {{0, 8000}, {1, 3000}, {1, 800}, {0, 400}};

class Synth {
public:
    // Called when a parameter request needs the host to restart the component (kRestart crossing 0.5).
    std::function<void()> on_restart_request;
    // Called when the parameter names changed (kHazRename crossing 0.5): the host must rescan.
    std::function<void()> on_rename;

    Synth() { reset(); }

    // Clears voices and every piece of filter state, including poison. Called on activate.
    void reset() {
        for (auto& v : voices_) v = Voice{};
        poisoned_ = false;
        for (std::uint32_t i = 0; i < kParamCount; ++i) values_[i] = kParams[i].def;
        rate_ = rate_ > 0 ? rate_ : 48000.0;
    }

    void activate(double sample_rate) {
        rate_ = sample_rate;
        // Re-activation restores the DSP state but keeps the parameter values the host has set.
        const auto kept = values_;
        reset();
        values_ = kept;
        poisoned_ = false;
    }

    [[nodiscard]] double value(ParamId id) const { return values_[id]; }

    void set_param(std::uint32_t id, double plain) {
        if (id >= kParamCount) return;
        const auto& p = kParams[id];
        plain = std::clamp(plain, p.min, p.max);
        if (p.stepped) plain = std::round(plain);
        const double old = values_[id];
        values_[id] = plain;
        if (id == kProgram && plain != old) {
            const auto& pr = kPrograms[static_cast<int>(plain)];
            values_[kWave] = pr.wave;
            values_[kCutoff] = pr.cutoff;
        }
        if (id == kRestart && old < 0.5 && plain >= 0.5 && on_restart_request) on_restart_request();
        if (id == kHazRename && (old >= 0.5) != (plain >= 0.5)) {
            renamed_ = plain >= 0.5;
            if (on_rename) on_rename();
        }
    }

    // Display name of a parameter: kHidden changes name with kHazRename.
    [[nodiscard]] const char* name(std::uint32_t id) const {
        if (id == kHidden && renamed_) return kRenamedHidden;
        return id < kParamCount ? kParams[id].name : "";
    }

    // Tuning is the CLAP note-expression value in semitones, or VST3 tuning converted to semitones.
    void note_on(int key, double velocity, std::int32_t note_id, double tuning_semitones = 0) {
        Voice* v = nullptr;
        for (auto& c : voices_)
            if (!c.active) {
                v = &c;
                break;
            }
        if (!v) v = &voices_[0];  // steal
        *v = Voice{};
        v->active = true;
        v->key = key;
        v->note_id = note_id;
        v->tuning = tuning_semitones;
        v->velocity = velocity;
        v->stage = Stage::Attack;
        const auto seed = static_cast<std::uint32_t>(values_[kLfoSeed]);
        if (seed == 0) {
            std::random_device rd;
            v->lfo_phase = std::uniform_real_distribution<double>(0.0, 1.0)(rd);
        } else {
            v->lfo_phase = std::fmod(static_cast<double>(seed) * 0.6180339887498949, 1.0);
        }
    }

    void note_off(int key, std::int32_t note_id) {
        for (auto& v : voices_)
            if (v.active && v.stage != Stage::Release && (note_id >= 0 ? v.note_id == note_id : v.key == key))
                v.stage = Stage::Release;
    }

    void note_tuning(int key, std::int32_t note_id, double semitones) {
        for (auto& v : voices_)
            if (v.active && (note_id >= 0 ? v.note_id == note_id : v.key == key)) v.tuning = semitones;
    }

    [[nodiscard]] bool any_voice_active() const {
        for (const auto& v : voices_)
            if (v.active) return true;
        return false;
    }

    [[nodiscard]] double release_tail_seconds() const { return values_[kRelease] / 1000.0; }

    void process(float* left, float* right, int frames) {
        // Hazards that act on the call itself.
        if (values_[kHazA] > 0.8 && values_[kHazB] > 0.9) {
            volatile int* bad = nullptr;
            *bad = 1;  // planted crash
        }
        if (values_[kHazHang] > 0.95) {
            for (;;) std::this_thread::sleep_for(std::chrono::seconds(3600));  // planted hang
        }
        if (values_[kHazPoison] > 0.9) poisoned_ = true;
        if (values_[kHazSticky] > 0.9) stuck_ = true;
        if (values_[kHazThrow] > 0.9) throw std::runtime_error("planted exception");

        const double level = std::pow(10.0, values_[kLevel] / 20.0);
        const bool nan_out = values_[kHazNan] > 0.9;
        const bool bypass = values_[kBypass] >= 0.5;
        const double cutoff = values_[kCutoff];
        const double g = 1.0 - std::exp(-2.0 * 3.141592653589793 * std::min(cutoff, rate_ * 0.45) / rate_);
        const double a_inc = 1.0 / std::max(1.0, values_[kAttack] * 0.001 * rate_);
        const double d_dec = (1.0 - values_[kSustain] / 100.0) / std::max(1.0, values_[kDecay] * 0.001 * rate_);
        const double sustain = values_[kSustain] / 100.0;
        const double r_dec = 1.0 / std::max(1.0, values_[kRelease] * 0.001 * rate_);
        const double lfo_inc = values_[kLfoRate] / rate_;
        double peak = 0;

        for (int n = 0; n < frames; ++n) {
            double mix = 0;
            for (auto& v : voices_) {
                if (!v.active) continue;
                switch (v.stage) {
                    case Stage::Attack:
                        v.env += a_inc;
                        if (v.env >= 1.0) { v.env = 1.0; v.stage = Stage::Decay; }
                        break;
                    case Stage::Decay:
                        v.env -= d_dec;
                        if (v.env <= sustain) { v.env = sustain; v.stage = Stage::Sustain; }
                        break;
                    case Stage::Sustain: v.env = sustain; break;
                    case Stage::Release:
                        v.env -= r_dec * std::max(v.env, 1e-3);
                        if (v.env <= 1e-4) { v.active = false; continue; }
                        break;
                }
                v.lfo_phase += lfo_inc;
                v.lfo_phase -= std::floor(v.lfo_phase);
                const double cents = values_[kDetune] + v.tuning * 100.0 +
                                     values_[kLfoDepth] * std::sin(6.283185307179586 * v.lfo_phase);
                const double freq = 440.0 * std::pow(2.0, (static_cast<double>(v.key) - 69.0) / 12.0 + cents / 1200.0);
                v.phase += freq / rate_;
                v.phase -= std::floor(v.phase);
                const double osc = values_[kWave] >= 0.5 ? 2.0 * v.phase - 1.0 : std::sin(6.283185307179586 * v.phase);
                v.lp += g * (osc - v.lp);
                mix += v.lp * v.env * v.velocity;
            }
            double out = mix * level;
            if (poisoned_ || stuck_) out = std::numeric_limits<double>::quiet_NaN();
            if (nan_out) out = std::numeric_limits<double>::quiet_NaN();
            if (bypass) out = 0;
            left[n] = right[n] = static_cast<float>(out);
            peak = std::max(peak, std::fabs(out));
        }
        values_[kMeter] = peak > 1e-5 ? std::max(-96.0, 20.0 * std::log10(peak)) : -96.0;
    }

private:
    enum class Stage { Attack, Decay, Sustain, Release };
    struct Voice {
        bool active = false;
        int key = 0;
        std::int32_t note_id = -1;
        double tuning = 0, velocity = 0, phase = 0, lfo_phase = 0, env = 0, lp = 0;
        Stage stage = Stage::Attack;
    };

    std::array<Voice, 8> voices_{};
    std::array<double, kParamCount> values_{};
    double rate_ = 48000.0;
    bool poisoned_ = false;
    bool stuck_ = false;    // never cleared: only a new instance is clean
    bool renamed_ = false;  // follows kHazRename, not part of the DSP state
};

}  // namespace mimicry::testsynth
