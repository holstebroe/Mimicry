#pragma once
// Tone harvest (docs/spec/06_TARGETS.md section 6a, WP5.13): for a harvested preset, play one pitch at one
// velocity several times, let each note ring until it has dropped below a threshold, and keep the consensus
// take, the one most like the others. The result is a training tone for the optimiser: preset in,
// WAV out.
//
// Works on any host::Instance (a WorkerClient in `mimicry tones`, CLAUDE.md rule 4). Every take renders
// from the complete preset state after a reset (rule 5); a failed or silent take is recorded, never
// dropped (rule 6).

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "core/Expected.hpp"
#include "host/PluginHost.hpp"
#include "opt/Embedding.hpp"

namespace mimicry::targets {

struct ToneOptions {
    int key = 48;
    int velocity = 100;           // MIDI 1..127
    int takes = 5;
    double gate_ms = 1000;        // key held
    double max_tail_ms = 8000;    // the renderer stops earlier once the plugin falls silent (-80 dB)
    double end_db = -60;          // the note has ended when its peak stays below this (dBFS) [D]
    double fade_ms = 30;          // fade-out over the end of a trimmed take
    double settle_ms = 250;
    int sample_rate = 48000;
    double max_spread = 2.0;      // consensus take further than this from the others: flagged unstable [D]
};

struct Take {
    bool ok = false;
    std::string why;              // outcome of a failed render, or "silent"
    double level_db = 0;          // embedding level of the take
    double distance = 0;          // mean distance to the other valid takes
    host::Audio audio;            // trimmed at the end threshold; empty if not ok
};

struct ToneResult {
    std::vector<Take> takes;
    int chosen = -1;              // index into takes, -1 if no take was usable
    double spread = 0;            // the chosen take's mean distance to the other valid takes
    bool unstable = false;        // spread > max_spread
    int valid = 0;                // takes that rendered and are not silent
    [[nodiscard]] const Take* tone() const { return chosen >= 0 ? &takes[static_cast<std::size_t>(chosen)] : nullptr; }
};

struct Consensus {
    int index = -1;               // the medoid among the valid takes, -1 if none
    double spread = 0;
    std::vector<double> mean_distance;  // per take (0 for invalid takes)
};

// The take with the smallest summed distance to the others (ties: the earliest). Distance of two takes: the
// embedding distance (level free) or the level difference in dB, whichever is larger, so takes that differ
// only in loudness still count (as the harvest verification, 06 section 6).
[[nodiscard]] Consensus pick_consensus(const std::vector<opt::Embedding>& embeddings, const std::vector<bool>& valid);

// Index one past the last 10 ms window whose peak is >= end_db, 0 if none (the take is silent).
[[nodiscard]] std::size_t note_end_frame(const host::Audio& audio, double end_db);

// Renders `options.takes` takes of one note from `state` and picks the consensus take.
[[nodiscard]] expected<ToneResult, std::string> render_tone(host::Instance& instance, const std::vector<std::uint8_t>& state,
                                                            const ToneOptions& options);

// A preset of a harvest library folder (`mimicry harvest` output, or any folder of .state files).
struct LibraryPreset {
    std::string stem;                   // file name without extension: "003-Warm Pad", the tone folder name
    std::string name;                   // the plugin's name for it, else the stem
    std::filesystem::path state;
    std::filesystem::path params;       // empty if there is none
};

// The `ok` presets of <dir>/index.json, or, without an index, every .state file in the folder (sorted).
[[nodiscard]] expected<std::vector<LibraryPreset>, std::string> list_library(const std::filesystem::path& dir);

// "C3_v100" (note name as the rest of Mimicry: C3 = MIDI 48).
[[nodiscard]] std::string tone_stem(int key, int velocity);

}  // namespace mimicry::targets
