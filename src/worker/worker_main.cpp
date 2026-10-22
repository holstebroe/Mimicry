// mimicry_worker: the only process that loads plugins (docs/spec/05_RESILIENCE.md section 1).
//
//   mimicry_worker --serve --shm-bytes N     speaks the supervisor protocol on descriptors 3 (in) and
//                                            4 (out), audio and state blobs in the shared memory on 5
//   mimicry_worker --version
//
// Messages are one JSON object per line (see src/supervisor/WorkerClient.cpp for the client side). A
// plugin that crashes or hangs takes this process down, not the supervisor.

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <format>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <optional>
#include <chrono>
#include <algorithm>

#include "core/Ipc.hpp"
#include "core/Json.hpp"
#include "core/Version.hpp"
#include "host/Codec.hpp"
#include "host/PluginHost.hpp"
#include "worker/EditorWindow.hpp"

#if defined(__linux__)
#include <sys/prctl.h>
#endif
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdlib>
#else
#include <unistd.h>
#endif
#include <csignal>

namespace {

using namespace mimicry;

constexpr int kInFd = 3, kOutFd = 4, kShmFd = 5;  // HandleMapping::to of the three inherited handles

json::Value error_reply(std::string_view message) {
    json::Value v;
    v.set("ok", false).set("error", message);
    return v;
}

json::Value ok_reply() {
    json::Value v;
    v.set("ok", true);
    return v;
}

class Server {
public:
    Server(ipc::LineChannel& channel, ipc::SharedMemory& shm)
        : channel_(channel), shm_(shm), payload_(shm.size() - ipc::kShmProgressBytes),
          blocks_(reinterpret_cast<std::uint64_t*>(static_cast<char*>(shm.data()) + payload_)) {}

    // Returns false when the loop should end.
    bool handle(const json::Value& msg) {
        const std::string& cmd = msg["cmd"].as_string();
        json::Value reply;
        try {
            if (cmd == "quit") {
                (void)channel_.write_line(ok_reply().dump());
                return false;
            }
            reply = dispatch(cmd, msg);
        } catch (const std::exception& e) {
            reply = error_reply(std::format("worker exception: {}", e.what()));
        } catch (...) {
            reply = error_reply("worker exception");
        }
        return channel_.write_line(reply.dump());
    }

    ~Server() {
        editor_.close();  // the plugin's view goes before the plugin
        instance_.reset();
    }

    void idle() {
        if (instance_) instance_->pump();
        if (editor_.is_open()) editor_step();
    }

private:
    json::Value describe() {
        json::Value v = ok_reply();
        v.set("info", host::to_json(instance_->info()));
        json::Array params;
        for (const auto& p : instance_->parameters()) params.push_back(host::to_json(p));
        v.set("params", std::move(params)).set("version", static_cast<double>(instance_->parameters_version()))
            .set("tail", instance_->tail_samples()).set("latency", instance_->latency_samples());
        return v;
    }

    json::Value dispatch(const std::string& cmd, const json::Value& msg) {
        if (cmd == "ping") return ok_reply();
        if (cmd == "open") {
            auto r = host::open(msg["path"].as_string(), msg["uid"].as_string());
            if (!r) return error_reply(r.error().message);
            instance_ = std::move(*r);
            instance_->set_block_counter(blocks_);
            return describe();
        }
        if (!instance_) return error_reply("no plugin is open");
        if (cmd == "describe") return describe();
        if (cmd == "value_text") {
            json::Value v = ok_reply();
            v.set("text", instance_->value_text(static_cast<std::uint32_t>(msg["id"].as_number()), msg["coord"].as_number()));
            return v;
        }
        if (cmd == "reset") {
            if (auto r = instance_->reset(); !r) return error_reply(r.error().message);
            return ok_reply();
        }
        if (cmd == "save_state") {
            auto blob = instance_->save_state();
            if (!blob) return error_reply(blob.error().message);
            if (blob->size() > payload_) return error_reply("state larger than the shared memory");
            std::memcpy(shm_.data(), blob->data(), blob->size());
            json::Value v = ok_reply();
            v.set("bytes", blob->size());
            return v;
        }
        if (cmd == "load_state") {
            const auto n = static_cast<std::size_t>(msg["bytes"].as_number());
            if (n > payload_) return error_reply("state larger than the shared memory");
            const auto* p = static_cast<const std::uint8_t*>(shm_.data());
            if (auto r = instance_->load_state(std::vector<std::uint8_t>(p, p + n)); !r) return error_reply(r.error().message);
            return ok_reply();
        }
        if (cmd == "render") return render(msg["job"], static_cast<std::size_t>(msg["state_bytes"].as_number()));
        if (cmd == "presets") {
            auto list = instance_->presets();
            if (!list) return error_reply(list.error().message);
            json::Array out;
            for (const auto& r : *list) out.push_back(host::to_json(r));
            json::Value v = ok_reply();
            v.set("presets", std::move(out));
            return v;
        }
        if (cmd.rfind("editor_", 0) == 0) return editor_command(cmd, msg);
        if (cmd == "apply_preset") {
            auto ref = host::preset_from_json(msg["ref"]);
            if (!ref) return error_reply(ref.error());
            auto cap = instance_->apply_preset(*ref, msg["wait_ms"].as_number(200));
            if (!cap) return error_reply(cap.error().message);
            if (cap->state.size() > payload_) return error_reply("state larger than the shared memory");
            std::memcpy(shm_.data(), cap->state.data(), cap->state.size());
            json::Array coords;
            for (double c : cap->coords) coords.emplace_back(c);
            json::Value v = ok_reply();
            v.set("bytes", cap->state.size()).set("coords", std::move(coords)).set("name", cap->name)
                .set("version", static_cast<double>(instance_->parameters_version()));
            return v;
        }
        return error_reply(std::format("unknown command '{}'", cmd));
    }

    // --- the editor session (docs/spec/06_TARGETS.md section 6) ---

    static std::string hash_of(const std::vector<std::uint8_t>& state) {  // as targets::state_hash
        std::uint64_t h = 1469598103934665603ull;
        for (const std::uint8_t b : state) {
            h ^= b;
            h *= 1099511628211ull;
        }
        return std::format("{:016x}-{}", h, state.size());
    }

    struct Capture {
        host::CapturedPreset preset;
        std::string hash;
    };

    // Takes the plugin's state as it is. False (with a status) when nothing changed since the last capture.
    bool do_capture(const std::string& wanted_name) {
        auto cap = instance_->capture_current(wanted_name);
        if (!cap) {
            editor_.set_status("capture failed: " + cap.error().message);
            events_.push_back("capture failed: " + cap.error().message);
            return false;
        }
        const std::string hash = hash_of(cap->state);
        for (const auto& c : captures_)
            if (c.hash == hash) {
                editor_.set_status(std::format("Same state as capture #{}: pick another preset first.", &c - captures_.data() + 1));
                last_capture_new_ = false;
                return false;
            }
        cap->name = wanted_name.empty() ? std::format("capture-{:03}", captures_.size() + 1) : wanted_name;
        captures_.push_back({std::move(*cap), hash});
        last_capture_new_ = true;
        editor_.set_status(std::format("Captured #{}: {}", captures_.size(), captures_.back().preset.name));
        events_.push_back(std::format("captured {}", captures_.size()));
        return true;
    }

    struct AutoRun {
        bool running = false, waiting = false, real_input = false;
        int max = 2000, misses = 0, clicks = 0;
        double wait_ms = 400;
        std::chrono::steady_clock::time_point deadline;
    };

    void start_auto(int max, double wait_ms) {
        if (!click_) {
            editor_.set_status("Teach the \"next preset\" click first.");
            return;
        }
        auto_ = AutoRun{};
        auto_.running = true;
        auto_.max = std::max(1, max);
        auto_.wait_ms = std::max(50.0, wait_ms);
        editor_.set_status("Auto-capture: clicking next and capturing...");
        events_.push_back("auto started");
        (void)do_capture({});  // the preset showing now is the first of the bank
    }

    void stop_auto(const std::string& why) {
        if (!auto_.running) return;
        auto_.running = false;
        editor_.set_status("Auto-capture stopped: " + why);
        events_.push_back("auto stopped: " + why);
    }

    // One step of the non-blocking state machines (called from the idle loop).
    void editor_step() {
        for (const std::string& action : editor_.take_actions()) {
            if (action == "capture") {
                (void)do_capture(editor_.take_name());
            } else if (action == "teach") {
                teach_now();
            } else if (action == "auto") {
                if (auto_.running) stop_auto("stopped by the user");
                else start_auto(auto_max_, auto_wait_ms_);
            } else if (action == "stop") {
                stop_auto("stopped by the user");
            } else if (action == "done") {
                finish_editor();
                return;
            }
        }
        if (editor_.take_plugin_closed()) {
            events_.push_back("the plugin closed its editor");
            finish_editor();
            return;
        }
        // Teaching: the click was recorded; once the plugin had time to react, check that it changed the state.
        if (teach_.stage == Teach::Stage::Armed) {
            if (auto c = editor_.take_click()) {
                teach_.click = *c;
                teach_.stage = Teach::Stage::Waiting;
                teach_.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(static_cast<int>(auto_wait_ms_));
                editor_.set_status("Click recorded; checking that it changed the preset...");
            }
        } else if (teach_.stage == Teach::Stage::Waiting && std::chrono::steady_clock::now() >= teach_.deadline) {
            teach_.stage = Teach::Stage::Off;
            auto cap = instance_->capture_current({});
            if (cap && hash_of(cap->state) != teach_.before_hash) {
                click_ = teach_.click;
                editor_.set_status("Click learned. Press Auto-capture to run through the bank.");
                events_.push_back("click taught");
            } else {
                editor_.set_status("That click changed nothing. Press Teach again and click the plugin's \"next preset\" button once.");
                events_.push_back("click changed nothing");
            }
        }
        // Auto-capture: click, wait, capture; stop when the bank wraps, nothing changes three times or --max.
        if (auto_.running) {
            const auto now = std::chrono::steady_clock::now();
            if (!auto_.waiting) {
                if (!editor_.replay_click(*click_, auto_.real_input)) {
                    stop_auto("the click could not be replayed");
                    return;
                }
                ++auto_.clicks;
                auto_.waiting = true;
                auto_.deadline = now + std::chrono::milliseconds(static_cast<int>(auto_.wait_ms));
            } else if (now >= auto_.deadline) {
                auto_.waiting = false;
                auto cap = instance_->capture_current({});
                if (!cap) {
                    stop_auto("capture failed: " + cap.error().message);
                } else {
                    const std::string hash = hash_of(cap->state);
                    const bool seen = std::any_of(captures_.begin(), captures_.end(), [&](const Capture& c) { return c.hash == hash; });
                    if (seen && !captures_.empty() && captures_.back().hash != hash) {
                        stop_auto("the bank wrapped around");
                    } else if (seen) {
                        // The click changed nothing (same as the last capture): after two posted tries the real mouse.
                        if (++auto_.misses == 2 && !auto_.real_input) {
                            auto_.real_input = true;
                            editor_.set_status("Posted clicks changed nothing; trying the real mouse...");
                        } else if (auto_.misses >= 3) {
                            stop_auto("three clicks in a row changed nothing");
                        }
                    } else {
                        auto_.misses = 0;
                        cap->name = std::format("capture-{:03}", captures_.size() + 1);
                        captures_.push_back({std::move(*cap), hash});
                        editor_.set_status(std::format("Auto-capture: {} presets", captures_.size()));
                        events_.push_back(std::format("captured {}", captures_.size()));
                        if (static_cast<int>(captures_.size()) >= auto_.max) stop_auto(std::format("reached {} presets", auto_.max));
                    }
                }
            }
        }
        editor_.set_counts(static_cast<int>(captures_.size()), click_.has_value(), auto_.running);
    }

    void teach_now() {
        auto cap = instance_->capture_current({});
        if (!cap) {
            editor_.set_status("capture failed: " + cap.error().message);
            return;
        }
        teach_ = Teach{};
        teach_.before_hash = hash_of(cap->state);
        teach_.stage = Teach::Stage::Armed;
        editor_.begin_teach();
        editor_.set_status("Click the plugin's \"next preset\" button once.");
    }

    void finish_editor() {
        auto_.running = false;
        editor_.cancel_teach();
        editor_.close();
        events_.push_back("closed");
    }

    json::Value editor_command(const std::string& cmd, const json::Value& msg) {
        if (cmd == "editor_open") {
            if (editor_.is_open()) return error_reply("the editor is already open");
            captures_.clear();
            events_.clear();
            click_.reset();
            auto_ = AutoRun{};
            teach_ = Teach{};
            if (msg["click"].is_object()) click_ = click_from_json(msg["click"]);
            auto size = editor_.open(*instance_, msg["title"].is_string() ? msg["title"].as_string() : instance_->info().name);
            if (!size) return error_reply(size.error());
            json::Value v = ok_reply();
            v.set("width", size->width).set("height", size->height).set("resizable", size->resizable);
            return v;
        }
        if (cmd == "editor_poll") {
            json::Value v = ok_reply();
            v.set("open", editor_.is_open()).set("captures", captures_.size()).set("teaching", teach_.stage != Teach::Stage::Off)
                .set("taught", click_.has_value()).set("auto_running", auto_.running)
                .set("signals", instance_->take_change_signals());
            if (click_) v.set("click", click_to_json(*click_));
            json::Array ev;
            for (auto& e : events_) ev.push_back(std::move(e));
            events_.clear();
            v.set("events", std::move(ev));
            return v;
        }
        if (cmd == "editor_take") {
            const auto i = static_cast<std::size_t>(msg["index"].as_number());
            if (i >= captures_.size()) return error_reply("no such capture");
            const auto& c = captures_[i].preset;
            if (c.state.size() > payload_) return error_reply("state larger than the shared memory");
            std::memcpy(shm_.data(), c.state.data(), c.state.size());
            json::Array coords;
            for (double u : c.coords) coords.emplace_back(u);
            json::Value v = ok_reply();
            v.set("bytes", c.state.size()).set("coords", std::move(coords)).set("name", c.name).set("hash", captures_[i].hash);
            return v;
        }
        if (cmd == "editor_action") {
            const std::string& action = msg["action"].as_string();
            if (!editor_.is_open()) return error_reply("the editor is not open");
            if (msg["max"].is_number()) auto_max_ = static_cast<int>(msg["max"].as_number());
            if (msg["wait_ms"].is_number()) auto_wait_ms_ = msg["wait_ms"].as_number();
            if (action == "capture") (void)do_capture(msg["name"].as_string());
            else if (action == "teach") teach_now();
            else if (action == "auto") start_auto(auto_max_, auto_wait_ms_);
            else if (action == "stop") stop_auto("stopped");
            else if (action == "done") finish_editor();
            else return error_reply("unknown editor action '" + action + "'");
            return ok_reply();
        }
        if (cmd == "editor_close") {
            if (editor_.is_open()) finish_editor();
            return ok_reply();
        }
        return error_reply(std::format("unknown command '{}'", cmd));
    }

    static json::Value click_to_json(const worker::ClickRecord& c) {
        json::Value v;
        v.set("fx", c.fx).set("fy", c.fy).set("class", c.window_class);
        return v;
    }
    static worker::ClickRecord click_from_json(const json::Value& v) {
        worker::ClickRecord c;
        c.fx = v["fx"].as_number();
        c.fy = v["fy"].as_number();
        c.window_class = v["class"].as_string();
        return c;
    }

    json::Value render(const json::Value& job_json, std::size_t state_bytes) {
        auto job = host::job_from_json(job_json);
        if (!job) return error_reply(job.error());
        if (state_bytes > payload_) return error_reply("state larger than the shared memory");
        if (state_bytes) {  // a source render from a harvested state, passed in the shared memory
            const auto* p = static_cast<const std::uint8_t*>(shm_.data());
            job->state.assign(p, p + state_bytes);
        }
        std::atomic_ref<std::uint64_t>(*blocks_).store(0);
        auto audio = instance_->render(*job);
        json::Value v = ok_reply();
        v.set("version", static_cast<double>(instance_->parameters_version()));
        v.set("message", instance_->last_message());
        if (!audio) {
            v.set("outcome", to_string(audio.error()));
            return v;
        }
        const std::size_t bytes = audio->samples.size() * sizeof(float);
        if (bytes > payload_) {
            v.set("outcome", to_string(Outcome::HostError));
            v.set("message", "rendered audio does not fit the shared memory");
            return v;
        }
        std::memcpy(shm_.data(), audio->samples.data(), bytes);
        v.set("outcome", to_string(Outcome::Ok)).set("frames", audio->samples.size() / static_cast<std::size_t>(audio->channels))
            .set("channels", audio->channels).set("rate", audio->sample_rate);
        return v;
    }

    ipc::LineChannel& channel_;
    ipc::SharedMemory& shm_;
    std::size_t payload_;
    std::uint64_t* blocks_;  // progress word at the end of the shared memory
    std::unique_ptr<host::Instance> instance_;

    // The editor session.
    worker::EditorWindow editor_;
    std::vector<Capture> captures_;
    std::vector<std::string> events_;
    std::optional<worker::ClickRecord> click_;
    bool last_capture_new_ = false;
    int auto_max_ = 2000;
    double auto_wait_ms_ = 400;
    AutoRun auto_;
    struct Teach {
        enum class Stage { Off, Armed, Waiting } stage = Stage::Off;
        std::string before_hash;
        worker::ClickRecord click;
        std::chrono::steady_clock::time_point deadline;
    } teach_;
};

int serve(std::size_t shm_bytes, const std::map<int, ipc::Handle>& handles) {
#if defined(_WIN32)
    // A crashing plugin must end the process at once: no error dialog, no Windows Error Reporting UI waiting for
    // a click (the supervisor would see a hang instead of a crash).
    ::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#else
    std::signal(SIGPIPE, SIG_IGN);
#if defined(__linux__)
    ::prctl(PR_SET_PDEATHSIG, SIGKILL);  // never outlive the supervisor (Windows: the supervisor's job object)
#endif
    // Plugins print to stdout; the protocol does not use it.
    ::dup2(2, 1);
#endif
    // Ctrl-C reaches the whole process group: the supervisor cancels gracefully (spec 04 section 10) and
    // ends its workers itself, so a worker must not die of it in the middle of a render.
    std::signal(SIGINT, SIG_IGN);
    auto handle = [&](int to) {
        const auto it = handles.find(to);
        return it == handles.end() ? ipc::kNoHandle : it->second;
    };
    auto shm = ipc::SharedMemory::from_handle(handle(kShmFd), shm_bytes);
    if (shm && shm->size() <= ipc::kShmProgressBytes) shm = mimicry::unexpected(std::string("shared memory too small"));
    if (!shm) {
        std::fputs(std::format("mimicry_worker: {}\n", shm.error()).c_str(), stderr);
        return 2;
    }
    ipc::LineChannel channel(handle(kInFd), handle(kOutFd));
    Server server(channel, *shm);

    std::string line;
    for (;;) {
        // The main thread pumps between jobs (docs/spec/02_HOSTING.md section 3).
        switch (channel.read_line(line, 10)) {
            case ipc::LineChannel::Read::Timeout: server.idle(); continue;
            case ipc::LineChannel::Read::Closed: return 0;
            case ipc::LineChannel::Read::Line: break;
        }
        auto msg = json::parse(line);
        if (!msg) {
            if (!channel.write_line(error_reply(msg.error()).dump())) return 0;
            continue;
        }
        if (!server.handle(*msg)) return 0;
    }
}

}  // namespace

int main(int argc, char** argv) {
    bool serve_mode = false;
    std::size_t shm_bytes = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        if (a == "--version") {
            std::fputs(std::format("mimicry_worker {}\n", mimicry::version).c_str(), stdout);
            return 0;
        }
        if (a == "--serve") serve_mode = true;
        else if (a == "--shm-bytes" && i + 1 < argc) shm_bytes = static_cast<std::size_t>(std::strtoull(argv[++i], nullptr, 10));
    }
    if (!serve_mode || shm_bytes == 0) {
        std::fputs("usage: mimicry_worker --serve --shm-bytes N   (started by mimicry; see docs/spec/05_RESILIENCE.md)\n", stderr);
        return 2;
    }
    return serve(shm_bytes, ipc::inherited_handles(argc, argv));
}
