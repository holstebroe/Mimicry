#include "supervisor/WorkerPool.hpp"

namespace mimicry::supervisor {

expected<std::unique_ptr<WorkerPool>, host::HostError> WorkerPool::open(const std::filesystem::path& plugin,
                                                                       std::string_view uid, int workers,
                                                                       WorkerOptions options) {
    auto pool = std::unique_ptr<WorkerPool>(new WorkerPool);
    const std::size_t n = static_cast<std::size_t>(std::max(workers, 1));
    pool->clients_.resize(n);
    pool->open_errors_.resize(n);
    for (std::size_t i = 0; i < n; ++i)
        pool->threads_.emplace_back(&WorkerPool::loop, pool.get(), i, plugin, std::string(uid), options);
    {
        std::unique_lock lock(pool->mutex_);
        pool->done_cv_.wait(lock, [&] { return pool->opened_ == static_cast<int>(n); });
    }
    for (std::size_t i = 0; i < n; ++i)
        if (!pool->clients_[i]) return unexpected(host::HostError{pool->open_errors_[i]});  // ~WorkerPool joins
    return pool;
}

WorkerPool::~WorkerPool() {
    {
        std::lock_guard lock(mutex_);
        quit_ = true;
    }
    work_cv_.notify_all();
    for (auto& t : threads_) t.join();
}

void WorkerPool::loop(std::size_t index, std::filesystem::path plugin, std::string uid, WorkerOptions options) {
    auto c = WorkerClient::open(plugin, uid, options);
    {
        std::lock_guard lock(mutex_);
        if (c) clients_[index] = std::move(*c);
        else open_errors_[index] = c.error().message;
        ++opened_;
    }
    done_cv_.notify_all();
    WorkerClient* client = clients_[index].get();
    std::uint64_t seen = 0;
    for (;;) {
        std::unique_lock lock(mutex_);
        work_cv_.wait(lock, [&] { return quit_ || batch_ != seen; });
        if (quit_) break;
        seen = batch_;
        // No client (open failed, the pool is being torn down), or a late wake-up after the batch ended.
        if (!client || !task_) continue;
        ++busy_;
        while (next_ < count_) {
            const std::size_t i = next_++;
            lock.unlock();
            (*task_)(*client, i);  // writes only its own result slot
            lock.lock();
        }
        if (--busy_ == 0) done_cv_.notify_all();
    }
    // The client (and its worker process) is destroyed on the thread that spawned it.
    std::lock_guard lock(mutex_);
    clients_[index].reset();
}

void WorkerPool::run(std::size_t count, const std::function<void(WorkerClient&, std::size_t)>& task) {
    if (count == 0) return;
    std::lock_guard batch_lock(batch_mutex_);
    std::unique_lock lock(mutex_);
    task_ = &task;
    count_ = count;
    next_ = 0;
    ++batch_;
    work_cv_.notify_all();
    // Done when every task was handed out and no thread is still running one.
    done_cv_.wait(lock, [&] { return next_ >= count && busy_ == 0; });
    task_ = nullptr;
    count_ = 0;
}

std::vector<expected<host::Audio, Outcome>> WorkerPool::render_all(const std::vector<host::Job>& jobs) {
    std::vector<expected<host::Audio, Outcome>> results(jobs.size(), unexpected(Outcome::HostError));
    run(jobs.size(), [&](WorkerClient& c, std::size_t i) { results[i] = c.render(jobs[i]); });
    return results;
}

std::string WorkerPool::value_text(std::uint32_t id, double coord) {
    std::string text;
    run(1, [&](WorkerClient& c, std::size_t) { text = c.value_text(id, coord); });
    return text;
}

WorkerStats WorkerPool::total_stats() const {
    WorkerStats t;
    for (const auto& c : clients_) {
        if (!c) continue;
        t.spawned += c->stats().spawned;
        t.crashes += c->stats().crashes;
        t.timeouts += c->stats().timeouts;
        t.recycled += c->stats().recycled;
    }
    return t;
}

}  // namespace mimicry::supervisor
