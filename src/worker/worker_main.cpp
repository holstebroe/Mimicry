// mimicry_worker: the only process that loads plugins (docs/spec/05_RESILIENCE.md section 1).
//
//   mimicry_worker --serve --shm-bytes N     speaks the supervisor protocol on descriptors 3 (in) and
//                                            4 (out), audio and state blobs in the shared memory on 5
//   mimicry_worker --version
//
// Messages are one JSON object per line (see src/supervisor/WorkerClient.cpp for the client side). A
// plugin that crashes or hangs takes this process down, not the supervisor.

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

#include "core/Ipc.hpp"
#include "core/Json.hpp"
#include "core/Version.hpp"
#include "host/Codec.hpp"
#include "host/PluginHost.hpp"

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
#include <csignal>
#include <unistd.h>
#endif

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

    void idle() {
        if (instance_) instance_->pump();
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
        if (cmd == "open_editor") {
            if (auto r = instance_->open_editor(); !r) return error_reply(r.error().message);
            return ok_reply();
        }
        if (cmd == "close_editor") {
            instance_->close_editor();
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
