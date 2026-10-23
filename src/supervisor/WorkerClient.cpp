#include "supervisor/WorkerClient.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <format>

#include "host/Codec.hpp"

#if !defined(_WIN32)
#include <csignal>
#endif

namespace mimicry::supervisor {

using host::HostError;

namespace {

// Longest audio a job can render: settle, the note plan and the whole tail (the worker renders up to
// tail_max_ms when the plugin does not go quiet).
double plan_seconds(const host::Job& job) {
    double end_ms = 0;
    for (const auto& n : job.notes) end_ms = std::max({end_ms, n.on_ms, n.off_ms});
    return (job.settle_ms + end_ms + std::max(job.tail_max_ms, 0.0)) / 1000.0;
}

// First difference between two parameter tables (id, name, steps, flags), or "" when they agree.
std::string table_difference(const std::vector<host::ParamDesc>& a, const std::vector<host::ParamDesc>& b) {
    if (a.size() != b.size()) return std::format("{} parameters became {}", a.size(), b.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        const auto& x = a[i];
        const auto& y = b[i];
        if (x.id != y.id || x.name != y.name)
            return std::format("parameter {} '{}' (id {}) is now '{}' (id {})", i, x.name, x.id, y.name, y.id);
        if (x.steps != y.steps || x.flags.readonly != y.flags.readonly || x.flags.program_change != y.flags.program_change ||
            x.flags.bypass != y.flags.bypass)
            return std::format("parameter '{}' (id {}) changed its steps or flags", x.name, x.id);
    }
    return {};
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
    stop_worker();
#if !defined(_WIN32)
    // Writing to a worker that just died must be an error return, not a fatal signal.
    static const bool sigpipe_ignored = (std::signal(SIGPIPE, SIG_IGN), true);
    (void)sigpipe_ignored;
#endif
    auto shm = ipc::SharedMemory::create(options_.shm_bytes);
    if (!shm) return unexpected(HostError{shm.error()});
    ipc::Handle to_read, to_write, from_read, from_write;
    if (!ipc::make_pipe(to_read, to_write)) return unexpected(HostError{"pipe failed"});
    if (!ipc::make_pipe(from_read, from_write)) {
        ipc::close_handle(to_read);
        ipc::close_handle(to_write);
        return unexpected(HostError{"pipe failed"});
    }

    auto proc = ipc::Process::spawn(options_.worker_exe, {"--serve", "--shm-bytes", std::to_string(options_.shm_bytes)},
                                    {{to_read, 3}, {from_write, 4}, {shm->handle(), 5}});
    // The child holds its own copies now; ours must go, or the worker's death would not close the pipe.
    ipc::close_handle(to_read);
    ipc::close_handle(from_write);
    if (!proc) {
        ipc::close_handle(to_write);
        ipc::close_handle(from_read);
        return unexpected(HostError{proc.error()});
    }
    process_ = std::move(*proc);
    channel_ = ipc::LineChannel(from_read, to_write);
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
}

void WorkerClient::adopt(const json::Value& d) {
    if (d["info"].is_object()) info_ = host::info_from_json(d["info"]);
    if (d["params"].is_array()) {
        std::vector<host::ParamDesc> now;
        for (const auto& p : d["params"].as_array()) now.push_back(host::param_from_json(p));
        if (!have_params_) {
            params_ = std::move(now);
            have_params_ = true;
        } else if (table_change_.empty()) {
            table_change_ = table_difference(params_, now);
        }
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

std::uint64_t WorkerClient::blocks_done() const {
    if (payload() == 0) return 0;
    return std::atomic_ref<std::uint64_t>(*reinterpret_cast<std::uint64_t*>(static_cast<char*>(shm_.data()) + payload())).load();
}

expected<host::Audio, Outcome> WorkerClient::render(const host::Job& job) {
    auto r = render_once(job);
    if (r) {
        if (options_.canary && (!canary_ || canary_->coords != job.coords || canary_->state != job.state)) {
        // Only pass or fail matters for the canary, and the checks fire during the notes: a short tail keeps
        // the check after every failure cheap.
        canary_ = job;
        canary_->tail_max_ms = std::min(canary_->tail_max_ms, options_.canary_tail_ms);
    }
        return r;
    }
    // A crash or timeout already cost the process; any other failure may have poisoned the instance in a way
    // its reset does not clear, so the last job that passed must pass again (spec 05 section 5).
    if (!options_.canary || !canary_ || !open_ || !table_change_.empty()) return r;
    const std::string why = message_;
    ++stats_.canaries;
    if (!render_once(*canary_)) {
        ++stats_.canary_restarts;
        (void)start_worker();
    }
    message_ = why;
    return r;
}

expected<host::Audio, Outcome> WorkerClient::render_once(const host::Job& job) {
    message_.clear();
    if (!table_change_.empty()) {
        message_ = "the plugin's parameter table changed: " + table_change_;
        return unexpected(Outcome::HostError);
    }
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
    if (job.state.size() > payload()) {
        message_ = "state larger than the shared memory";
        return unexpected(Outcome::HostError);
    }
    if (!job.state.empty()) std::memcpy(shm_.data(), job.state.data(), job.state.size());
    json::Value req, reply;
    req.set("cmd", "render").set("job", host::to_json(job)).set("state_bytes", job.state.size());
    std::string detail;
    switch (call(req, timeout, reply, detail)) {
        case Call::Ok: break;
        case Call::Timeout: {
            ++stats_.timeouts;
            open_ = false;
            const auto blocks = blocks_done();
            // Spec 05 section 9: a plugin waiting for a dialog, a licence check or a message loop never
            // reaches its first block.
            message_ = blocks == 0 ? std::format("no answer within {:.1f} s: worker killed; the plugin never reached the "
                                                 "first block (stuck in reset or activation: a dialog, a licence check "
                                                 "or a missing message loop?)", timeout)
                                   : std::format("no answer within {:.1f} s: worker killed in block {}", timeout, blocks);
            return unexpected(Outcome::Timeout);
        }
        case Call::Crashed:
            ++stats_.crashes;
            open_ = false;
            message_ = std::format("worker died in block {}: {}", blocks_done(), detail);
            return unexpected(Outcome::Crash);
    }
    ++renders_since_start_;
    if (!reply["ok"].as_bool()) {
        message_ = reply["error"].as_string();
        return unexpected(Outcome::HostError);
    }
    message_ = reply["message"].as_string();
    // A new parameter version (restart request with a rescan): fetch the table and compare it with the
    // first one. A changed table fails this and every later render (spec 05 section 9).
    if (const auto v = static_cast<std::uint64_t>(reply["version"].as_number(static_cast<double>(version_))); v != version_) {
        json::Value dreq, desc;
        dreq.set("cmd", "describe");
        std::string ddetail;
        if (call(dreq, options_.call_timeout_s, desc, ddetail) != Call::Ok) {
            open_ = false;
            message_ = "worker lost while re-reading the parameter table: " + ddetail;
            return unexpected(Outcome::HostError);
        }
        if (desc["ok"].as_bool()) adopt(desc);
        version_ = v;
        if (!table_change_.empty()) {
            message_ = "the plugin's parameter table changed: " + table_change_;
            return unexpected(Outcome::HostError);
        }
    }
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
    if (count * sizeof(float) > payload() || audio.channels < 1) {
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
    if (n > payload()) return unexpected(HostError{"bad state size"});
    const auto* p = static_cast<const std::uint8_t*>(shm_.data());
    return std::vector<std::uint8_t>(p, p + n);
}

expected<void, HostError> WorkerClient::load_state(const std::vector<std::uint8_t>& blob) {
    if (blob.size() > payload()) return unexpected(HostError{"state larger than the shared memory"});
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

expected<std::vector<host::PresetRef>, HostError> WorkerClient::presets() {
    if (!open_) {
        if (auto r = start_worker(); !r) return unexpected(r.error());
    }
    json::Value req, reply;
    req.set("cmd", "presets");
    std::string detail;
    // Crawling a large preset folder can take a while.
    if (call(req, options_.open_timeout_s, reply, detail) != Call::Ok) {
        open_ = false;
        return unexpected(HostError{"worker lost while listing presets: " + detail});
    }
    if (!reply["ok"].as_bool()) return unexpected(HostError{reply["error"].as_string()});
    std::vector<host::PresetRef> out;
    for (const auto& r : reply["presets"].as_array()) {
        auto ref = host::preset_from_json(r);
        if (!ref) return unexpected(HostError{ref.error()});
        out.push_back(std::move(*ref));
    }
    return out;
}

expected<host::CapturedPreset, HostError> WorkerClient::apply_preset(const host::PresetRef& ref, double wait_ms) {
    if (!table_change_.empty()) return unexpected(HostError{"the plugin's parameter table changed: " + table_change_});
    if (!open_) {
        if (auto r = start_worker(); !r) return unexpected(r.error());
    }
    json::Value req, reply;
    req.set("cmd", "apply_preset").set("ref", host::to_json(ref)).set("wait_ms", wait_ms);
    std::string detail;
    switch (call(req, options_.call_timeout_s + 0.02 * wait_ms, reply, detail)) {
        case Call::Ok: break;
        case Call::Timeout:
            ++stats_.timeouts;
            open_ = false;
            return unexpected(HostError{"timeout: no answer while loading the preset; worker killed"});
        case Call::Crashed:
            ++stats_.crashes;
            open_ = false;
            return unexpected(HostError{"crash: the worker died while loading the preset (" + detail + ")"});
    }
    if (!reply["ok"].as_bool()) return unexpected(HostError{reply["error"].as_string()});
    // A preset load may make the plugin rescan its table; such a change is final, as for renders.
    if (const auto v = static_cast<std::uint64_t>(reply["version"].as_number(static_cast<double>(version_))); v != version_) {
        json::Value dreq, desc;
        dreq.set("cmd", "describe");
        if (call(dreq, options_.call_timeout_s, desc, detail) == Call::Ok && desc["ok"].as_bool()) adopt(desc);
        version_ = v;
        if (!table_change_.empty()) return unexpected(HostError{"the plugin's parameter table changed: " + table_change_});
    }
    const auto n = static_cast<std::size_t>(reply["bytes"].as_number());
    if (n > payload()) return unexpected(HostError{"bad state size"});
    host::CapturedPreset cap;
    const auto* p = static_cast<const std::uint8_t*>(shm_.data());
    cap.state.assign(p, p + n);
    for (const auto& c : reply["coords"].as_array()) cap.coords.push_back(c.as_number());
    cap.name = reply["name"].as_string();
    if (cap.coords.size() != params_.size()) return unexpected(HostError{"the worker returned a different parameter count"});
    return cap;
}

expected<host::EditorSize, HostError> WorkerClient::editor_open(const std::string& title, const json::Value& click) {
    if (!open_) {
        if (auto r = start_worker(); !r) return unexpected(r.error());
    }
    json::Value req, reply;
    req.set("cmd", "editor_open").set("title", title);
    if (click.is_object()) req.set("click", click);
    std::string detail;
    if (call(req, options_.open_timeout_s, reply, detail) != Call::Ok) {
        open_ = false;
        return unexpected(HostError{"worker lost while opening the editor: " + detail});
    }
    if (!reply["ok"].as_bool()) return unexpected(HostError{reply["error"].as_string()});
    host::EditorSize size;
    size.width = static_cast<int>(reply["width"].as_number());
    size.height = static_cast<int>(reply["height"].as_number());
    size.resizable = reply["resizable"].as_bool();
    return size;
}

expected<WorkerClient::EditorStatus, HostError> WorkerClient::editor_poll() {
    if (!open_) return unexpected(HostError{"no worker"});
    json::Value req, reply;
    req.set("cmd", "editor_poll");
    std::string detail;
    if (call(req, options_.call_timeout_s, reply, detail) != Call::Ok) {
        open_ = false;
        return unexpected(HostError{"the worker was lost during the editor session: " + detail});
    }
    if (!reply["ok"].as_bool()) return unexpected(HostError{reply["error"].as_string()});
    EditorStatus st;
    st.open = reply["open"].as_bool();
    st.teaching = reply["teaching"].as_bool();
    st.taught = reply["taught"].as_bool();
    st.auto_running = reply["auto_running"].as_bool();
    st.captures = static_cast<int>(reply["captures"].as_number());
    st.signals = static_cast<int>(reply["signals"].as_number());
    st.click = reply["click"];
    for (const auto& e : reply["events"].as_array()) st.events.push_back(e.as_string());
    return st;
}

expected<host::CapturedPreset, HostError> WorkerClient::editor_take(int index, std::string* hash) {
    if (!open_) return unexpected(HostError{"no worker"});
    json::Value req, reply;
    req.set("cmd", "editor_take").set("index", index);
    std::string detail;
    if (call(req, options_.call_timeout_s, reply, detail) != Call::Ok) {
        open_ = false;
        return unexpected(HostError{"the worker was lost during the editor session: " + detail});
    }
    if (!reply["ok"].as_bool()) return unexpected(HostError{reply["error"].as_string()});
    const auto n = static_cast<std::size_t>(reply["bytes"].as_number());
    if (n > payload()) return unexpected(HostError{"bad state size"});
    host::CapturedPreset cap;
    const auto* p = static_cast<const std::uint8_t*>(shm_.data());
    cap.state.assign(p, p + n);
    for (const auto& c : reply["coords"].as_array()) cap.coords.push_back(c.as_number());
    cap.name = reply["name"].as_string();
    if (hash) *hash = reply["hash"].as_string();
    if (cap.coords.size() != params_.size()) return unexpected(HostError{"the worker returned a different parameter count"});
    return cap;
}

expected<void, HostError> WorkerClient::editor_action(const std::string& action, const json::Value& options) {
    if (!open_) return unexpected(HostError{"no worker"});
    json::Value req, reply;
    req.set("cmd", "editor_action").set("action", action);
    for (const auto& [k, v] : options.as_object()) req.set(k, v);
    std::string detail;
    if (call(req, options_.call_timeout_s, reply, detail) != Call::Ok) {
        open_ = false;
        return unexpected(HostError{"the worker was lost during the editor session: " + detail});
    }
    if (!reply["ok"].as_bool()) return unexpected(HostError{reply["error"].as_string()});
    return {};
}

void WorkerClient::editor_close() {
    if (!open_) return;
    json::Value req, reply;
    req.set("cmd", "editor_close");
    std::string detail;
    (void)call(req, options_.call_timeout_s, reply, detail);
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

expected<void, HostError> WorkerClient::open_editor() {
    if (!open_) {
        if (auto r = start_worker(); !r) return r;
    }
    json::Value req, reply;
    req.set("cmd", "open_editor");
    std::string detail;
    if (call(req, options_.call_timeout_s, reply, detail) != Call::Ok) {
        open_ = false;
        return unexpected(HostError{"worker lost while opening the editor: " + detail});
    }
    if (!reply["ok"].as_bool()) return unexpected(HostError{reply["error"].as_string()});
    return {};
}

void WorkerClient::close_editor() {
    if (!open_) return;
    json::Value req, reply;
    req.set("cmd", "close_editor");
    std::string detail;
    if (call(req, options_.call_timeout_s, reply, detail) != Call::Ok) open_ = false;
}

}  // namespace mimicry::supervisor
