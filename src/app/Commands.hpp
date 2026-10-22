#pragma once
#include <filesystem>

#include "app/Cli.hpp"
#include "core/Expected.hpp"
#include "host/PluginHost.hpp"

namespace mimicry::app {

// Exit codes: 0 done, 1 failed, 2 not implemented, 3 replay not reproduced, 4 cancelled (a resumable run).
inline constexpr int kExitCancelled = 4;

// Harvest on an open instance (also the first step of `transfer`): the library under `root`.
struct HarvestSummary {
    std::filesystem::path dir;
    int ok = 0;
    bool cancelled = false;
};
[[nodiscard]] expected<HarvestSummary, std::string> harvest_to_library(host::Instance& inst, const Args& a, const std::filesystem::path& root);

int cmd_fit(const Args& a);
int cmd_eval(const Args& a);
int cmd_runs(const Args& a);
int cmd_cancel(const Args& a);
int cmd_harvest(const Args& a);
int cmd_transfer(const Args& a);

}  // namespace mimicry::app
