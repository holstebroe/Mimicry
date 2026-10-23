#pragma once
// The shared render core (docs/spec/02_HOSTING.md section 5, WP1.4): turns a format Engine into an
// Instance. Reset, parameters at time 0, settle, sample-exact notes, tail detection and the output checks
// of spec 05 section 2 are written once here, for CLAP and VST3 alike.

#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

#include "host/Engine.hpp"
#include "host/MessagePump.hpp"

namespace mimicry::host {

// One long-lived thread that runs the plugin's process() calls, so the plugin sees a stable audio thread.
class RenderThread {
public:
    RenderThread();
    ~RenderThread();
    RenderThread(const RenderThread&) = delete;
    RenderThread& operator=(const RenderThread&) = delete;

    [[nodiscard]] std::thread::id id() const { return thread_.get_id(); }
    // Runs fn on the render thread and waits for it.
    void run(const std::function<void()>& fn);

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    const std::function<void()>* task_ = nullptr;
    bool done_ = false;
    bool quit_ = false;
    std::thread thread_;
};

class Renderer final : public Instance {
public:
    // `roles` is the same object the engine was created with.
    Renderer(std::unique_ptr<Engine> engine, std::shared_ptr<ThreadRoles> roles);
    ~Renderer() override;

    // Brings the engine up at the default rate. Called once by open().
    [[nodiscard]] expected<void, HostError> initialise(int sample_rate = 48000, int block_size = 512);

    [[nodiscard]] const PluginInfo& info() const override { return engine_->info(); }
    [[nodiscard]] const std::vector<ParamDesc>& parameters() const override { return engine_->parameters(); }
    [[nodiscard]] std::string value_text(std::uint32_t id, double coord) const override;
    [[nodiscard]] expected<Audio, Outcome> render(const Job& job) override;
    [[nodiscard]] const std::string& last_message() const override { return message_; }
    [[nodiscard]] int tail_samples() const override { return engine_->tail_samples(); }
    [[nodiscard]] int latency_samples() const override { return engine_->latency_samples(); }
    [[nodiscard]] std::uint64_t parameters_version() const override { return version_; }
    void pump() override;
    void set_block_counter(std::uint64_t* counter) override { blocks_ = counter; }

    [[nodiscard]] expected<std::vector<std::uint8_t>, HostError> save_state() override;
    [[nodiscard]] expected<void, HostError> load_state(const std::vector<std::uint8_t>& blob) override;
    [[nodiscard]] expected<void, HostError> reset() override;
    [[nodiscard]] expected<std::vector<PresetRef>, HostError> presets() override;
    [[nodiscard]] expected<CapturedPreset, HostError> apply_preset(const PresetRef& ref, double wait_ms = 200) override;

    [[nodiscard]] expected<void, HostError> open_editor() override;
    void close_editor() override;
    [[nodiscard]] bool editor_open() const override { return engine_->editor_open(); }

    // Reset before every render (default). Turning it off is for profiling only: a candidate must
    // start from a clean state (CLAUDE.md rule 5).
    void set_reset_each_render(bool on) { reset_each_render_ = on; }

private:
    [[nodiscard]] expected<void, HostError> restart_engine();
    [[nodiscard]] expected<Audio, Outcome> render_once(const Job& job);
    [[nodiscard]] Outcome fail(Outcome outcome, std::string message);
    // After a failed render (spec 05 section 5): reset, then load the clean state taken at start.
    void recover();
    // Pending main-thread callbacks, then a requested restart: re-enumerate and bump the version.
    [[nodiscard]] expected<void, HostError> handle_restart_request();
    // Silent blocks for `frames` frames; the first block carries `first` and the program change. Output is
    // discarded (preset jobs).
    [[nodiscard]] expected<void, HostError> run_silence(int frames, std::span<const ParamValue> first, int program, int bank);

    std::unique_ptr<Engine> engine_;
    std::shared_ptr<ThreadRoles> roles_;
    RenderThread thread_;
    MessagePump pump_;
    int rate_ = 48000;
    int max_block_ = 512;
    bool started_ = false;
    bool reset_each_render_ = true;
    std::uint64_t version_ = 1;
    std::string message_;
    std::vector<std::uint8_t> clean_;  // state right after the first activation
    bool have_clean_ = false;
    bool preset_unreported_ = false;  // the last asynchronous preset load ended without loaded / on_error
    std::uint64_t* blocks_ = nullptr;
};

}  // namespace mimicry::host
