#include "supervisor/WorkerPool.hpp"

#include <atomic>
#include <thread>

namespace mimicry::supervisor {

expected<std::unique_ptr<WorkerPool>, host::HostError> WorkerPool::open(const std::filesystem::path& plugin,
                                                                       std::string_view uid, int workers,
                                                                       WorkerOptions options) {
    auto pool = std::unique_ptr<WorkerPool>(new WorkerPool);
    for (int i = 0; i < std::max(workers, 1); ++i) {
        auto c = WorkerClient::open(plugin, uid, options);
        if (!c) return unexpected(c.error());
        pool->clients_.push_back(std::move(*c));
    }
    return pool;
}

std::vector<expected<host::Audio, Outcome>> WorkerPool::render_all(const std::vector<host::Job>& jobs) {
    std::vector<expected<host::Audio, Outcome>> results(jobs.size(), unexpected(Outcome::HostError));
    std::atomic<std::size_t> next{0};
    std::vector<std::thread> threads;
    for (auto& client : clients_) {
        threads.emplace_back([&, c = client.get()] {
            for (std::size_t i = next++; i < jobs.size(); i = next++) results[i] = c->render(jobs[i]);
        });
    }
    for (auto& t : threads) t.join();
    return results;
}

WorkerStats WorkerPool::total_stats() const {
    WorkerStats t;
    for (const auto& c : clients_) {
        t.spawned += c->stats().spawned;
        t.crashes += c->stats().crashes;
        t.timeouts += c->stats().timeouts;
        t.recycled += c->stats().recycled;
    }
    return t;
}

}  // namespace mimicry::supervisor
