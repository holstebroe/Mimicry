#pragma once
// A run directory (docs/spec/07_OUTPUT_VALIDATION.md section 1, spec 09 section 3): `run.json` says what the
// run is and where it stands, `checkpoint.json` lets a stopped run continue. The status is written at every
// change, atomically, so a killed process leaves a readable state ("running" with a dead process is an
// interrupted run: resumable like a cancelled one).

#include <filesystem>
#include <string>
#include <vector>

#include "app/Cli.hpp"
#include "core/Expected.hpp"
#include "core/Json.hpp"

namespace mimicry::app {

class RunDir {
public:
    // runs/<timestamp>-<slug>/ under `root`, run.json with status "running".
    [[nodiscard]] static expected<RunDir, std::string> create(const std::filesystem::path& root, const std::string& kind,
                                                              const std::string& name, const Args& args);
    // An existing run directory (run.json must be there).
    [[nodiscard]] static expected<RunDir, std::string> open(const std::filesystem::path& dir);

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return dir_; }
    [[nodiscard]] const json::Value& meta() const noexcept { return meta_; }
    [[nodiscard]] Args saved_args() const { return Args::from_json(meta_["args"]); }
    [[nodiscard]] std::filesystem::path checkpoint_path() const { return dir_ / "checkpoint.json"; }
    [[nodiscard]] std::filesystem::path cancel_path() const { return dir_ / "cancel"; }

    // Writes run.json with the new status ("running", "cancelled", "finished", "failed") and the members of
    // `extra` (loss_db, evals, elapsed_s, message, ...).
    void set_status(const std::string& status, const json::Value& extra = {});
    // Merges members without touching the status.
    void update(const json::Value& extra);

private:
    void write();
    std::filesystem::path dir_;
    json::Value meta_;
};

// Summary of one run directory for `mimicry runs` and the GUI.
struct RunInfo {
    std::filesystem::path dir;
    std::string kind, status, plugin, target, updated;
    double loss_db = 0, elapsed_s = 0, time_cap_s = 0;
    long evals = 0;
    bool resumable = false;
    json::Value meta;
};

// Every run directory under `root` (newest first).
[[nodiscard]] std::vector<RunInfo> list_runs(const std::filesystem::path& root);

}  // namespace mimicry::app
