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
//   ignore_tuning >= 0.5       note tuning (CLAP note expression, VST3 NoteOnEvent.tuning) is ignored, so only
//                              MIDI pitch bend moves the pitch (spec 02 section 5 calibration). Hidden
//   bend_range                 pitch-bend range in semitones (default 2). Hidden
//   require_offline >= 0.5     output turns NaN unless the host set offline rendering (CLAP render extension,
//                              VST3 kOffline through clap-wrapper): T16 "offline mode set". Hidden
//   factory bank               six presets (load_factory) for the harvest test T17: CLAP preset discovery +
//                              preset-load, MIDI program change (the VST3 build's program list). "Init" is the
//                              defaults (the planted no-op), "Lead Copy" equals "Lead" (the planted duplicate)
//   smoothing  (ms)            one-pole glide of Level and Cutoff from their defaults after (re)activation,
//                              like a parameter smoother; 0 = none (default). The settle-time probe (spec 05
//                              section 6) must measure it. Hidden

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
    kIgnoreTuning,  // hidden
    kBendRange,     // hidden
    kRequireOffline,  // hidden
    kSmoothing,       // hidden
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
    {kIgnoreTuning, "Ignore Tuning", "Global", "", 0, 1, 0, true, true, false, false, Scale::Linear},
    {kBendRange, "Bend Range", "Global", "", 1, 24, 2, true, true, false, false, Scale::Linear},
    {kRequireOffline, "Require Offline", "Global", "", 0, 1, 0, true, true, false, false, Scale::Linear},
    {kSmoothing, "Smoothing", "Global", "ms", 0, 2000, 0, false, true, false, false, Scale::Linear},
};

inline constexpr const char* kRenamedHidden = "Hidden Drive (renamed)";

// The factory bank (spec 07 section 5): values over the defaults. Parameters not named keep their default.
struct FactoryValue {
    ParamId id;
    double value;
};
struct FactoryPreset {
    const char* name;
    FactoryValue values[8];
    int count;
};
inline constexpr FactoryPreset kFactoryBank[] = {
    {"Init", {}, 0},
    {"Soft Pad", {{kCutoff, 1200}, {kAttack, 400}, {kDecay, 600}, {kSustain, 80}, {kRelease, 1200}, {kLevel, -10}}, 6},
    {"Lead", {{kWave, 1}, {kCutoff, 6000}, {kAttack, 2}, {kDecay, 150}, {kSustain, 60}, {kRelease, 200}, {kDetune, 7}}, 7},
    {"Pluck", {{kWave, 1}, {kCutoff, 2500}, {kAttack, 1}, {kDecay, 120}, {kSustain, 0}, {kRelease, 100}, {kLevel, -6}}, 7},
    {"Lead Copy", {{kWave, 1}, {kCutoff, 6000}, {kAttack, 2}, {kDecay, 150}, {kSustain, 60}, {kRelease, 200}, {kDetune, 7}}, 7},
    {"Vibrato", {{kCutoff, 9000}, {kAttack, 20}, {kLfoRate, 6}, {kLfoDepth, 30}}, 4},
};
inline constexpr int kFactoryCount = static_cast<int>(sizeof kFactoryBank / sizeof kFactoryBank[0]);

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
        bend_ = 0;
        sm_level_ = kParams[kLevel].def;  // a smoother starts from the plugin's initial values
        sm_cutoff_ = kParams[kCutoff].def;
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

    // A factory preset replaces every value (as a real preset does); out-of-range indices are ignored.
    bool load_factory(int index) {
        if (index < 0 || index >= kFactoryCount) return false;
        for (std::uint32_t i = 0; i < kParamCount; ++i)
            if (!kParams[i].readonly && i != kHazRename) values_[i] = kParams[i].def;
        const auto& f = kFactoryBank[index];
        for (int k = 0; k < f.count; ++k) values_[f.values[k].id] = f.values[k].value;
        return true;
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
        v->tuning = values_[kIgnoreTuning] >= 0.5 ? 0.0 : tuning_semitones;
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

    // The host's render mode (CLAP render extension).
    void set_offline(bool offline) { offline_ = offline; }

    // MIDI pitch bend, -1..1 of the bend range (channel-wide).
    void pitch_bend(double amount) { bend_ = std::clamp(amount, -1.0, 1.0); }

    void note_off(int key, std::int32_t note_id) {
        for (auto& v : voices_)
            if (v.active && v.stage != Stage::Release && (note_id >= 0 ? v.note_id == note_id : v.key == key))
                v.stage = Stage::Release;
    }

    void note_tuning(int key, std::int32_t note_id, double semitones) {
        for (auto& v : voices_)
            if (v.active && (note_id >= 0 ? v.note_id == note_id : v.key == key))
                v.tuning = values_[kIgnoreTuning] >= 0.5 ? 0.0 : semitones;
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

        double level = std::pow(10.0, values_[kLevel] / 20.0);
        const bool nan_out = values_[kHazNan] > 0.9 || (values_[kRequireOffline] >= 0.5 && !offline_);
        const bool bypass = values_[kBypass] >= 0.5;
        const double cutoff = values_[kCutoff];
        double g = 1.0 - std::exp(-2.0 * 3.141592653589793 * std::min(cutoff, rate_ * 0.45) / rate_);
        const double smooth_ms = values_[kSmoothing];
        const double smooth_a = smooth_ms > 0 ? 1.0 - std::exp(-1000.0 / (smooth_ms * rate_)) : 1.0;
        const double a_inc = 1.0 / std::max(1.0, values_[kAttack] * 0.001 * rate_);
        const double d_dec = (1.0 - values_[kSustain] / 100.0) / std::max(1.0, values_[kDecay] * 0.001 * rate_);
        const double sustain = values_[kSustain] / 100.0;
        const double r_dec = 1.0 / std::max(1.0, values_[kRelease] * 0.001 * rate_);
        const double lfo_inc = values_[kLfoRate] / rate_;
        double peak = 0;

        for (int n = 0; n < frames; ++n) {
            if (smooth_ms > 0) {  // per-sample glide of level (dB) and cutoff (Hz, in log)
                sm_level_ += smooth_a * (values_[kLevel] - sm_level_);
                sm_cutoff_ *= std::pow(cutoff / sm_cutoff_, smooth_a);
                level = std::pow(10.0, sm_level_ / 20.0);
                g = 1.0 - std::exp(-2.0 * 3.141592653589793 * std::min(sm_cutoff_, rate_ * 0.45) / rate_);
            }
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
                const double cents = values_[kDetune] + v.tuning * 100.0 + bend_ * values_[kBendRange] * 100.0 +
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
    double bend_ = 0;
    bool offline_ = false;  // host render mode: not DSP state, survives reset
    double sm_level_ = 0, sm_cutoff_ = 1000;  // smoother state (kSmoothing)
};

}  // namespace mimicry::testsynth
