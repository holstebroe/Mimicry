#pragma once
// Preset harvest (docs/spec/06_TARGETS.md section 6, WP5.8): the ladder of methods, each loaded preset
// verified against the defaults, the earlier presets and itself, and a library of verified states.
//
// Works on any host::Instance: a WorkerClient in `mimicry harvest` (the plugin stays in a worker, CLAUDE.md
// rule 4), an in-process instance for debugging. A failed load is an outcome, never dropped (rule 6), and
// nothing is written into the plugin's own folders (rule 8).

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/Expected.hpp"
#include "core/Json.hpp"
#include "host/PluginHost.hpp"
#include "opt/Profile.hpp"
#include "output/PresetExport.hpp"

namespace mimicry::targets {

// Outcome of one preset (spec 06 section 6).
enum class PresetOutcome { Ok, NoChange, Duplicate, NotReproducible, LoadError, Timeout };
[[nodiscard]] std::string_view to_string(PresetOutcome o) noexcept;

struct HarvestOptions {
    std::vector<std::filesystem::path> from;   // --from: preset files and folders (ladder step 1)
    bool raw_files = false;                    // --raw-files: any file in those folders is tried as a state blob
    bool standard_folders = true;              // VST3: the standard preset folders for the vendor and plugin
    std::optional<host::PresetMethod> method;  // --method: only this method
    bool all_methods = false;                  // every method, not only until one yields presets
    std::optional<opt::HarvestRecipe> recipe;  // from the profile: tried first
    host::Job plan;                            // verification render (notes, rate); coords and state are set here
    double wait_ms = 200;                      // after each load (spec 02 section 7)
    int max_presets = 0;                       // ok presets to collect, 0 = all
    int abandon_after = 3;                     // a method whose first N presets are all no-change is abandoned [D]
    int stop_after = 8;                        // MIDI program change: N empty slots in a row end the bank [D]
    std::function<void(const struct HarvestEntry&)> progress;
    // Polled before every preset: true stops the harvest with what it has (`HarvestResult::cancelled`).
    std::function<bool()> cancel;
    // Entries of an earlier, stopped harvest (`load_library`): their presets are not tried again, the ok ones
    // keep their numbers and count for the duplicate check and for --max.
    std::vector<struct HarvestEntry> prior;
};

struct HarvestEntry {
    int number = 0;                // 1-based number in the library (ok presets only), else 0
    host::PresetRef ref;
    std::string name;              // the plugin's name, else "<method>-NNN"
    PresetOutcome outcome = PresetOutcome::LoadError;
    std::string message;
    std::string state_hash;
    int duplicate_of = 0;          // number of the earlier preset
    double difference = 0;         // render difference to the defaults (embedding units, dB-like)
    bool params_reproduce = false; // a render from the parameter snapshot matches the render from the state
    host::CapturedPreset captured; // kept for ok presets
    int position = 0, of = 0;      // 1-based position of this preset among the `of` the method offers
    // Set on entries read back from a library: files already written (names in the library folder).
    std::string state_file, params_file;
};

struct MethodReport {
    host::PresetMethod method;
    int offered = 0, tried = 0, ok = 0;
    std::string note;              // why it stopped early, if it did
    int resumed = 0;               // presets taken over from an earlier session
};

struct HarvestResult {
    std::vector<HarvestEntry> entries;
    std::vector<MethodReport> methods;
    std::optional<opt::HarvestRecipe> recipe;  // the method that yielded presets (the first one, by ladder order)
    double floor = 0;                          // render difference between two renders of the defaults
    double threshold = 0;                      // "same sound" below this
    std::string listing_error;                 // the plugin's preset listing failed (file presets were still tried)
    bool cancelled = false;                    // options.cancel stopped the harvest; the library holds what was found
    [[nodiscard]] int ok_count() const;
};

// Ladder order (spec 06 section 6): files, the plugin's catalogue, program lists, MIDI program change,
// program parameters.
[[nodiscard]] int ladder_rank(host::PresetMethod m) noexcept;

// Classifies presets against the defaults and the ones accepted before (spec 06 section 6): the loaded
// state must differ from the defaults and from earlier presets, and the state blob alone must reproduce the
// sound. Used by `harvest` and by `mimicry capture`, which gets its presets from the plugin's own window.
class PresetVerifier {
public:
    [[nodiscard]] static expected<PresetVerifier, std::string> create(host::Instance& instance, const host::Job& plan);
    PresetVerifier(PresetVerifier&&) noexcept;
    PresetVerifier& operator=(PresetVerifier&&) noexcept;
    ~PresetVerifier();
    [[nodiscard]] double floor() const;
    [[nodiscard]] double threshold() const;
    // An ok entry of an earlier session (its number and state hash set): counts for the duplicate check.
    // False when its state no longer renders.
    bool adopt(const HarvestEntry& entry);
    // Sets outcome, message, state_hash, difference, duplicate_of and params_reproduce of `entry`; an ok
    // preset takes over `captured`. The caller sets the number and calls accept() for ok presets.
    void verify(HarvestEntry& entry, host::CapturedPreset& captured);
    void accept(const HarvestEntry& entry);

private:
    PresetVerifier();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// File presets for step 1: the given files, the preset files in the given folders (.vstpreset, .state, CLAP
// state files; any file with `raw_files`), and for VST3 the standard folders of the vendor and plugin.
[[nodiscard]] std::vector<host::PresetRef> file_presets(const HarvestOptions& options, const host::PluginInfo& info);

[[nodiscard]] expected<HarvestResult, std::string> harvest(host::Instance& instance, const HarvestOptions& options);

// presets/<format>-<uid>-<hash>/ under `root`: NNN-<name>.state and .params.json per ok preset, index.json (every
// preset with its method, ref, state hash and outcome) and harvest-report.csv. Returns the directory.
[[nodiscard]] expected<std::filesystem::path, std::string> write_library(const std::filesystem::path& root,
                                                                         const HarvestResult& result,
                                                                         const host::PluginInfo& info,
                                                                         const std::vector<host::ParamDesc>& params,
                                                                         const output::ValueText& text);

// The library written by an earlier harvest of this plugin (`dir` from write_library): every entry of its
// index.json, ok presets with their state. Feeds HarvestOptions::prior.
[[nodiscard]] expected<std::vector<HarvestEntry>, std::string> load_library(const std::filesystem::path& dir);

// A preset name found inside a state blob: plugins that keep their preset in a key/value tree (JUCE ValueTree
// and similar) store its name as a string next to a key such as "presetName" or "displayName". Used by
// `capture` when the plugin does not name its presets itself [D]. Empty when nothing plausible is found.
[[nodiscard]] std::string guess_preset_name(const std::vector<std::uint8_t>& state);

// FNV-1a 64 of a state blob, hex.
[[nodiscard]] std::string state_hash(const std::vector<std::uint8_t>& state);

}  // namespace mimicry::targets
