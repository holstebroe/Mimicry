#include "supervisor/WorkerClient.hpp"

#include <algorithm>
#include <cstring>
#include <format>

#include "host/Codec.hpp"

#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>

#include <csignal>
#endif

namespace mimicry::supervisor {

using host::HostError;

namespace {

// Nominal audio duration of a job: settle plus the note plan (the tail is open-ended).
double plan_seconds(const host::Job& job) {
    double end_ms = 0;
    for (const auto& n : job.notes) end_ms = std::max({end_ms, n.on_ms, n.off_ms});
    return (job.settle_ms + end_ms) / 1000.0;
}

}  // namespace

expected<std::unique_ptr<WorkerClient>, HostError> WorkerClient::open(const std::filesystem::path& plugin,
                                                                      std::string_view uid, WorkerOptions options) {
    std::unique_ptr<WorkerClient> c(new WorkerClient);
    c->plugin_ = plugin;
    c->uid_ = uid;
    c->options_ = std::move(options);
    if (c->options_.worker_exe.empty()) {
        c->options_.worker_exe = ipc::executable_dir() /
#if defined(_WIN32)
                                 "mimicry_worker.exe";
#else
                                 "mimicry_worker";
#endif
    }
    if (auto r = c->start_worker(); !r) return unexpected(r.error());
    return c;
}

WorkerClient::~WorkerClient() { stop_worker(); }

void WorkerClient::stop_worker() {
    if (process_.id() > 0) {
        std::string ignored;
        json::Value reply;
        json::Value quit;
        quit.set("cmd", "quit");
        // Ask politely, then insist.
        if (channel_.write_line(quit.dump())) {
            std::string line;
            (void)channel_.read_line(line, 500);
        }
        channel_.close();
        if (process_.running()) (void)process_.kill_and_reap();
    }
    channel_.close();
    shm_ = ipc::SharedMemory{};
    open_ = false;
    renders_since_start_ = 0;
}

expected<void, HostError> WorkerClient::start_worker() {
#if defined(_WIN32)
    return unexpected(HostError{"process isolation is not implemented on Windows yet (plan WP1.5)"});
#else
    stop_worker();
    // Writing to a worker that just died must be an error return, not a fatal signal.
    static const bool sigpipe_ignored = (std::signal(SIGPIPE, SIG_IGN), true);
    (void)sigpipe_ignored;
    auto shm = ipc::SharedMemory::create(options_.shm_bytes);
    if (!shm) return unexpected(HostError{shm.error()});
    int to_worker[2], from_worker[2];
    if (::pipe(to_worker) != 0) return unexpected(HostError{"pipe failed"});
    if (::pipe(from_worker) != 0) {
        ::close(to_worker[0]);
        ::close(to_worker[1]);
        return unexpected(HostError{"pipe failed"});
    }
    for (const int fd : {to_worker[0], to_worker[1], from_worker[0], from_worker[1]}) ::fcntl(fd, F_SETFD, FD_CLOEXEC);

    auto proc = ipc::Process::spawn(options_.worker_exe, {"--serve", "--shm-bytes", std::to_string(options_.shm_bytes)},
                                    {{to_worker[0], 3}, {from_worker[1], 4}, {shm->fd(), 5}});
    ::close(to_worker[0]);
    ::close(from_worker[1]);
    if (!proc) {
        ::close(to_worker[1]);
        ::close(from_worker[0]);
        return unexpected(HostError{proc.error()});
    }
    process_ = std::move(*proc);
    channel_ = ipc::LineChannel(from_worker[0], to_worker[1]);
    shm_ = std::move(*shm);
    ++stats_.spawned;

    json::Value open;
    open.set("cmd", "open").set("path", plugin_.string()).set("uid", uid_);
    json::Value reply;
    std::string detail;
    switch (call(open, options_.open_timeout_s, reply, detail)) {
        case Call::Ok: break;
        case Call::Timeout: return unexpected(HostError{"the plugin did not finish loading in time"});
        case Call::Crashed: return unexpected(HostError{std::format("the worker died while loading the plugin ({})", detail)});
    }
    if (!reply["ok"].as_bool()) {
        const std::string msg = reply["error"].as_string();
        stop_worker();
        return unexpected(HostError{msg});
    }
    adopt(reply);
    open_ = true;
    return {};
#endif
}

void WorkerClient::adopt(const json::Value& d) {
    if (d["info"].is_object()) info_ = host::info_from_json(d["info"]);
    if (d["params"].is_array()) {
        params_.clear();
        for (const auto& p : d["params"].as_array()) params_.push_back(host::param_from_json(p));
    }
    tail_ = static_cast<int>(d["tail"].as_number());
    latency_ = static_cast<int>(d["latency"].as_number());
    version_ = static_cast<std::uint64_t>(d["version"].as_number(1));
}

WorkerClient::Call WorkerClient::call(const json::Value& request, double timeout_s, json::Value& reply,
                                      std::string& detail) {
    if (!channel_.write_line(request.dump())) {
        detail = process_.kill_and_reap();
        channel_.close();
        return Call::Crashed;
    }
    std::string line;
    switch (channel_.read_line(line, static_cast<int>(std::min(timeout_s, 1e6) * 1000))) {
        case ipc::LineChannel::Read::Line: break;
        case ipc::LineChannel::Read::Timeout:
            detail = process_.kill_and_reap();
            channel_.close();
            return Call::Timeout;
        case ipc::LineChannel::Read::Closed:
            detail = process_.reap();
            channel_.close();
            return Call::Crashed;
    }
    auto parsed = json::parse(line);
    if (!parsed) {
        detail = "bad reply: " + parsed.error();
        (void)process_.kill_and_reap();
        channel_.close();
        return Call::Crashed;
    }
    reply = std::move(*parsed);
    return Call::Ok;
}

std::string WorkerClient::value_text(std::uint32_t id, double coord) const {
    auto* self = const_cast<WorkerClient*>(this);
    if (!self->open_ && !self->start_worker()) return {};
    json::Value req, reply;
    req.set("cmd", "value_text").set("id", id).set("coord", coord);
    std::string detail;
    if (self->call(req, options_.call_timeout_s, reply, detail) != Call::Ok) {
        self->open_ = false;
        return {};
    }
    return reply["text"].as_string();
}

expected<host::Audio, Outcome> WorkerClient::render(const host::Job& job) {
    message_.clear();
    // Recycle the process every N renders, and bring a dead one back (the plugin is re-opened from scratch).
    if (open_ && renders_since_start_ >= options_.recycle_after) {
        ++stats_.recycled;
        if (auto r = start_worker(); !r) {
            message_ = r.error().message;
            return unexpected(Outcome::HostError);
        }
    } else if (!open_) {
        if (auto r = start_worker(); !r) {
            message_ = r.error().message;
            return unexpected(Outcome::HostError);
        }
    }

    const double timeout = std::max(options_.min_timeout_s, options_.timeout_factor * plan_seconds(job));
    json::Value req, reply;
    req.set("cmd", "render").set("job", host::to_json(job));
    std::string detail;
    switch (call(req, timeout, reply, detail)) {
        case Call::Ok: break;
        case Call::Timeout:
            ++stats_.timeouts;
            open_ = false;
            message_ = std::format("no answer within {:.1f} s: worker killed", timeout);
            return unexpected(Outcome::Timeout);
        case Call::Crashed:
            ++stats_.crashes;
            open_ = false;
            message_ = std::format("worker died: {}", detail);
            return unexpected(Outcome::Crash);
    }
    ++renders_since_start_;
    if (!reply["ok"].as_bool()) {
        message_ = reply["error"].as_string();
        return unexpected(Outcome::HostError);
    }
    message_ = reply["message"].as_string();
    version_ = static_cast<std::uint64_t>(reply["version"].as_number(static_cast<double>(version_)));
    auto outcome = host::outcome_from_string(reply["outcome"].as_string());
    if (!outcome) {
        message_ = outcome.error();
        return unexpected(Outcome::HostError);
    }
    if (*outcome != Outcome::Ok) return unexpected(*outcome);

    host::Audio audio;
    audio.channels = static_cast<int>(reply["channels"].as_number());
    audio.sample_rate = static_cast<int>(reply["rate"].as_number());
    const auto frames = static_cast<std::size_t>(reply["frames"].as_number());
    const std::size_t count = frames * static_cast<std::size_t>(std::max(audio.channels, 0));
    if (count * sizeof(float) > shm_.size() || audio.channels < 1) {
        message_ = "worker reported more audio than the shared memory holds";
        return unexpected(Outcome::HostError);
    }
    audio.samples.resize(count);
    std::memcpy(audio.samples.data(), shm_.data(), count * sizeof(float));
    return audio;
}

expected<std::vector<std::uint8_t>, HostError> WorkerClient::save_state() {
    if (!open_) {
        if (auto r = start_worker(); !r) return unexpected(r.error());
    }
    json::Value req, reply;
    req.set("cmd", "save_state");
    std::string detail;
    if (call(req, options_.call_timeout_s, reply, detail) != Call::Ok) {
        open_ = false;
        return unexpected(HostError{"worker lost during save_state: " + detail});
    }
    if (!reply["ok"].as_bool()) return unexpected(HostError{reply["error"].as_string()});
    const auto n = static_cast<std::size_t>(reply["bytes"].as_number());
    if (n > shm_.size()) return unexpected(HostError{"bad state size"});
    const auto* p = static_cast<const std::uint8_t*>(shm_.data());
    return std::vector<std::uint8_t>(p, p + n);
}

expected<void, HostError> WorkerClient::load_state(const std::vector<std::uint8_t>& blob) {
    if (blob.size() > shm_.size()) return unexpected(HostError{"state larger than the shared memory"});
    if (!open_) {
        if (auto r = start_worker(); !r) return unexpected(r.error());
    }
    std::memcpy(shm_.data(), blob.data(), blob.size());
    json::Value req, reply;
    req.set("cmd", "load_state").set("bytes", blob.size());
    std::string detail;
    if (call(req, options_.call_timeout_s, reply, detail) != Call::Ok) {
        open_ = false;
        return unexpected(HostError{"worker lost during load_state: " + detail});
    }
    if (!reply["ok"].as_bool()) return unexpected(HostError{reply["error"].as_string()});
    return {};
}

expected<void, HostError> WorkerClient::reset() {
    if (!open_) return start_worker();
    json::Value req, reply;
    req.set("cmd", "reset");
    std::string detail;
    if (call(req, options_.call_timeout_s, reply, detail) != Call::Ok) {
        open_ = false;
        return unexpected(HostError{"worker lost during reset: " + detail});
    }
    if (!reply["ok"].as_bool()) return unexpected(HostError{reply["error"].as_string()});
    return {};
}

}  // namespace mimicry::supervisor
