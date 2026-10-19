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
#if !defined(_WIN32)
#include <csignal>
#include <unistd.h>
#endif

namespace {

using namespace mimicry;

constexpr int kInFd = 3, kOutFd = 4, kShmFd = 5;

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
        if (cmd == "render") return render(msg["job"]);
        return error_reply(std::format("unknown command '{}'", cmd));
    }

    json::Value render(const json::Value& job_json) {
        auto job = host::job_from_json(job_json);
        if (!job) return error_reply(job.error());
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

int serve(std::size_t shm_bytes) {
#if defined(_WIN32)
    (void)shm_bytes;
    std::fputs("mimicry_worker: process isolation is not implemented on Windows yet\n", stderr);
    return 2;
#else
    std::signal(SIGPIPE, SIG_IGN);
#if defined(__linux__)
    ::prctl(PR_SET_PDEATHSIG, SIGKILL);  // never outlive the supervisor
#endif
    // Plugins print to stdout; the protocol does not use it.
    ::dup2(2, 1);

    auto shm = ipc::SharedMemory::from_fd(kShmFd, shm_bytes);
    if (shm && shm->size() <= ipc::kShmProgressBytes) shm = mimicry::unexpected(std::string("shared memory too small"));
    if (!shm) {
        std::fputs(std::format("mimicry_worker: {}\n", shm.error()).c_str(), stderr);
        return 2;
    }
    ipc::LineChannel channel(kInFd, kOutFd);
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
#endif
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
    return serve(shm_bytes);
}
