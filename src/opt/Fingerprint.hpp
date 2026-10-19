#pragma once
// Fingerprint database (docs/spec/04_OPTIMIZER.md section 7) [A]: random valid presets of one plugin,
// rendered once, stored as parameter vector + embedding. A new target starts from its nearest neighbours,
// which turns S1 from random into informed. Keyed like the profile (format + uid + version hash) and
// stores the parameter ids and names, so a database is never applied to a different parameter table.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "core/Expected.hpp"
#include "host/PluginHost.hpp"
#include "opt/Embedding.hpp"
#include "opt/Objective.hpp"
#include "opt/Space.hpp"

namespace mimicry::opt {

struct FingerprintDb {
    host::PluginFormat format = host::PluginFormat::Clap;
    std::string uid, version_hash;
    std::vector<std::uint32_t> ids;
    std::vector<std::string> names;
    std::vector<std::vector<float>> coords;   // full vectors, one entry per parameter
    std::vector<Embedding> embeddings;
    long rendered = 0, failed = 0;            // how many random presets were tried and failed

    [[nodiscard]] std::size_t size() const noexcept { return coords.size(); }
    // True when the database was built for exactly this plugin and parameter table.
    [[nodiscard]] bool matches(const host::PluginInfo& info, const std::vector<host::ParamDesc>& params) const;
    // Indices of the k entries nearest to `e` (embedding distance), nearest first.
    [[nodiscard]] std::vector<std::size_t> nearest(const Embedding& e, std::size_t k) const;

    // Binary file: "MIMFPDB1", a JSON header line, then float32 rows (coords then embedding).
    [[nodiscard]] expected<void, std::string> save(const std::filesystem::path& path) const;
    [[nodiscard]] static expected<FingerprintDb, std::string> load(const std::filesystem::path& path);
    [[nodiscard]] static std::filesystem::path default_path(const std::filesystem::path& dir, const host::PluginInfo& info);
};

struct FingerprintOptions {
    host::Job job;            // note plan template
    long count = 2000;        // random presets to render
    long batch = 64;
    std::uint64_t seed = 1;
    int threads = 1;          // embedding threads
    std::function<void(long done, long total)> progress;
};

// Renders `count` Latin-hypercube presets inside the safe box (categoricals uniform, fixed parameters at
// default) and keeps the valid ones.
[[nodiscard]] FingerprintDb build_fingerprints(const RenderBatch& render, const host::PluginInfo& info, const Space& space,
                                               const FingerprintOptions& options);

}  // namespace mimicry::opt
