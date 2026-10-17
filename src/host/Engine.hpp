#pragma once
// The per-format primitive the shared render core drives (docs/spec/02_HOSTING.md sections 2, 3, 5).
//
// A backend implements Engine in src/host/clap or src/host/vst3 and nothing else; the render loop is
// written once in Renderer. Calls are made from two threads, as the spec's thread model says: `activate`,
// `deactivate`, enumeration, state and destruction on the main thread; `start`, `process` and `stop`
// on the render thread.

#include <atomic>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "host/PluginHost.hpp"

namespace mimicry::host {

// Who is the main thread and who is the audio thread, reported truthfully to the plugin.
struct ThreadRoles {
    std::thread::id main;
    std::atomic<std::thread::id> audio{};
};

struct ParamValue {
    std::uint32_t id;
    double coord;  // Mimicry coordinate u in [0,1]
};

// A note event at a frame offset inside the current block.
struct NoteEvent {
    std::uint32_t frame = 0;
    bool on = true;
    int key = 60;
    double cents = 0;
    float velocity = 0.8f;
    std::int32_t note_id = 0;
};

struct Block {
    int frames = 0;
    std::span<const ParamValue> params;  // applied at frame 0
    std::span<const NoteEvent> notes;    // sorted by frame
};

class Engine {
public:
    virtual ~Engine() = default;

    [[nodiscard]] virtual const PluginInfo& info() const = 0;
    [[nodiscard]] virtual const std::vector<ParamDesc>& parameters() const = 0;
    [[nodiscard]] virtual std::string value_text(std::uint32_t id, double coord) const = 0;

    // main thread
    [[nodiscard]] virtual expected<void, HostError> activate(int sample_rate, int max_block) = 0;
    virtual void deactivate() = 0;
    [[nodiscard]] virtual expected<std::vector<std::uint8_t>, HostError> save_state() = 0;
    [[nodiscard]] virtual expected<void, HostError> load_state(const std::vector<std::uint8_t>& blob) = 0;
    // Re-reads the parameter table after a restart request; true if it changed.
    [[nodiscard]] virtual bool rescan_parameters() = 0;
    // True once if the plugin asked for a restart since the last call.
    [[nodiscard]] virtual bool take_restart_request() = 0;
    // Main thread, before block 0: lets a backend mirror the values on its controller object (VST3
    // setParamNormalized, spec 02 section 2). The values still reach the plugin through the block.
    virtual void apply_controller_values(std::span<const ParamValue>) {}
    // Runs plugin callbacks that were requested for the main thread (clap on_main_thread, VST3 runloop).
    virtual void pump_main_thread() {}

    // render thread
    [[nodiscard]] virtual expected<void, HostError> start() = 0;
    virtual void stop() = 0;
    // Renders `block.frames` stereo frames (a mono plugin is mixed to both channels).
    [[nodiscard]] virtual expected<void, HostError> process(const Block& block, float* left, float* right) = 0;

    [[nodiscard]] virtual int tail_samples() const = 0;
    [[nodiscard]] virtual int latency_samples() const = 0;
};

// Backend entry points. Each returns an error if the format is not compiled in.
[[nodiscard]] expected<std::unique_ptr<Engine>, HostError> open_clap_engine(const std::filesystem::path& path,
                                                                           std::string_view uid,
                                                                           std::shared_ptr<ThreadRoles> roles);
[[nodiscard]] expected<std::unique_ptr<Engine>, HostError> open_vst3_engine(const std::filesystem::path& path,
                                                                           std::string_view uid,
                                                                           std::shared_ptr<ThreadRoles> roles);

}  // namespace mimicry::host
