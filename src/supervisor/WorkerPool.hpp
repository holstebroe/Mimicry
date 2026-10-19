#pragma once
// A pool of isolated workers rendering candidate jobs in parallel (docs/spec/01_ARCHITECTURE.md section 4).
//
// Each client lives on its own long-lived pool thread, which opens it and makes every call that may
// (re)spawn its worker process. On Linux the worker's parent-death signal (PR_SET_PDEATHSIG) belongs to the
// thread that spawned it, so a worker respawned from a short-lived thread would be killed when that thread
// ends, and the next candidate would be scored as a Crash.

#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "supervisor/WorkerClient.hpp"

namespace mimicry::supervisor {

class WorkerPool {
public:
    // Opens `workers` workers on the same plugin. Fails if any cannot load the plugin.
    [[nodiscard]] static expected<std::unique_ptr<WorkerPool>, host::HostError> open(
        const std::filesystem::path& plugin, std::string_view uid, int workers, WorkerOptions options = {});
    ~WorkerPool();
    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;

    [[nodiscard]] int size() const { return static_cast<int>(clients_.size()); }
    // Direct access for calls that do not restart a worker (info, parameters); renders go through
    // render_all so a respawn happens on the client's pool thread.
    [[nodiscard]] WorkerClient& client(int i) { return *clients_[static_cast<std::size_t>(i)]; }

    // Renders all jobs, results in job order. A crash or timeout costs only that candidate. One batch at a
    // time; safe to call from any thread.
    [[nodiscard]] std::vector<expected<host::Audio, Outcome>> render_all(const std::vector<host::Job>& jobs);

    [[nodiscard]] WorkerStats total_stats() const;

private:
    WorkerPool() = default;
    void loop(std::size_t index, std::filesystem::path plugin, std::string uid, WorkerOptions options);

    std::vector<std::unique_ptr<WorkerClient>> clients_;
    std::vector<std::thread> threads_;
    std::mutex batch_mutex_;  // serialises render_all
    std::mutex mutex_;
    std::condition_variable work_cv_, done_cv_;
    const std::vector<host::Job>* jobs_ = nullptr;
    std::vector<expected<host::Audio, Outcome>>* results_ = nullptr;
    std::size_t next_ = 0;
    std::uint64_t batch_ = 0;
    int busy_ = 0;
    int opened_ = 0;
    bool quit_ = false;
    std::vector<std::string> open_errors_;
};

}  // namespace mimicry::supervisor
