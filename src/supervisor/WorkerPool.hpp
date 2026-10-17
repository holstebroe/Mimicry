#pragma once
// A pool of isolated workers rendering candidate jobs in parallel (docs/spec/01_ARCHITECTURE.md section 4).

#include <memory>
#include <vector>

#include "supervisor/WorkerClient.hpp"

namespace mimicry::supervisor {

class WorkerPool {
public:
    // Opens `workers` workers on the same plugin. Fails if the first one cannot load the plugin.
    [[nodiscard]] static expected<std::unique_ptr<WorkerPool>, host::HostError> open(
        const std::filesystem::path& plugin, std::string_view uid, int workers, WorkerOptions options = {});

    [[nodiscard]] int size() const { return static_cast<int>(clients_.size()); }
    [[nodiscard]] WorkerClient& client(int i) { return *clients_[static_cast<std::size_t>(i)]; }

    // Renders all jobs, results in job order. A crash or timeout costs only that candidate.
    [[nodiscard]] std::vector<expected<host::Audio, Outcome>> render_all(const std::vector<host::Job>& jobs);

    [[nodiscard]] WorkerStats total_stats() const;

private:
    std::vector<std::unique_ptr<WorkerClient>> clients_;
};

}  // namespace mimicry::supervisor
