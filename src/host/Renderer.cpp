#include "host/Renderer.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <exception>
#include <format>
#include <fstream>
#include <iterator>
#include <thread>

#include "host/ParamUtil.hpp"

namespace mimicry::host {

// --- RenderThread ----------------------------------------------------------------------------

RenderThread::RenderThread() {
    thread_ = std::thread([this] {
        std::unique_lock lock(mutex_);
        for (;;) {
            cv_.wait(lock, [this] { return quit_ || task_ != nullptr; });
            if (quit_) return;
            const auto* task = task_;
            lock.unlock();
            (*task)();
            lock.lock();
            task_ = nullptr;
            done_ = true;
            cv_.notify_all();
        }
    });
}

RenderThread::~RenderThread() {
    {
        std::lock_guard lock(mutex_);
        quit_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void RenderThread::run(const std::function<void()>& fn) {
    std::unique_lock lock(mutex_);
    done_ = false;
    task_ = &fn;
    cv_.notify_all();
    cv_.wait(lock, [this] { return done_; });
    done_ = false;
}

// --- Engine defaults -------------------------------------------------------------------------

expected<void, HostError> Engine::load_preset(const PresetRef& ref) {
    if (ref.method != PresetMethod::File)
        return unexpected(HostError{std::format("preset method '{}' is not supported by this plugin", to_string(ref.method))});
    std::ifstream f(ref.location, std::ios::binary);
    if (!f) return unexpected(HostError{std::format("cannot read '{}'", ref.location)});
    const std::vector<std::uint8_t> blob((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return load_state(blob);
}

// --- Renderer --------------------------------------------------------------------------------

namespace {

constexpr double kSilenceDb = -90.0;  // spec 05 section 2
constexpr double kQuietDb = -80.0;    // tail detection, spec 02 section 5
constexpr double kQuietSeconds = 0.2;

template <class F>
expected<void, HostError> guarded(const char* what, F&& fn) {
    try {
        return fn();
    } catch (const std::exception& e) {
        return unexpected(HostError{std::format("{}: plugin threw '{}'", what, e.what())});
    } catch (...) {
        return unexpected(HostError{std::format("{}: plugin threw an unknown exception", what)});
    }
}

int frames_for(double ms, int rate) { return static_cast<int>(std::llround(ms * 0.001 * rate)); }

}  // namespace

Renderer::Renderer(std::unique_ptr<Engine> engine, std::shared_ptr<ThreadRoles> roles)
    : engine_(std::move(engine)), roles_(std::move(roles)) {
    roles_->audio.store(thread_.id());
}

Renderer::~Renderer() {
    if (started_) thread_.run([this] { try { engine_->stop(); } catch (...) {} });
    try {
        engine_->deactivate();
    } catch (...) {
    }
    engine_.reset();  // destroys the plugin on the main thread
}

expected<void, HostError> Renderer::initialise(int sample_rate, int block_size) {
    rate_ = sample_rate;
    max_block_ = block_size;
    auto a = guarded("activate", [&] { return engine_->activate(rate_, max_block_); });
    if (!a) return a;
    expected<void, HostError> s;
    thread_.run([&] { s = guarded("start", [&] { return engine_->start(); }); });
    if (!s) {
        engine_->deactivate();
        return s;
    }
    started_ = true;
    if (!have_clean_) {
        // The clean snapshot of spec 05 section 5; a plugin without state support has nothing to reload.
        have_clean_ = true;
        try {
            if (auto st = engine_->save_state()) clean_ = std::move(*st);
        } catch (...) {
        }
    }
    return {};
}

std::string Renderer::value_text(std::uint32_t id, double coord) const {
    try {
        return engine_->value_text(id, coord);
    } catch (...) {
        return {};  // spec 05 section 9: a plugin exception never crosses into the host
    }
}

void Renderer::recover() {
    if (!restart_engine()) return;
    if (!clean_.empty()) (void)load_state(clean_);
}

expected<void, HostError> Renderer::handle_restart_request() {
    try {
        engine_->pump_main_thread();
    } catch (...) {
    }
    if (!engine_->take_restart_request()) return {};
    if (engine_->rescan_parameters()) ++version_;
    return restart_engine();
}

void Renderer::pump() {
    pump_.pump();
    try {
        engine_->pump_main_thread();
    } catch (...) {
    }
}

expected<std::vector<std::uint8_t>, HostError> Renderer::save_state() {
    try {
        return engine_->save_state();
    } catch (const std::exception& e) {
        return unexpected(HostError{std::format("save_state: plugin threw '{}'", e.what())});
    } catch (...) {
        return unexpected(HostError{"save_state: plugin threw"});
    }
}

expected<void, HostError> Renderer::load_state(const std::vector<std::uint8_t>& blob) {
    return guarded("load_state", [&] { return engine_->load_state(blob); });
}

// stop, deactivate, activate, start (docs/spec/02_HOSTING.md section 2, "Reset").
expected<void, HostError> Renderer::restart_engine() {
    if (started_) {
        thread_.run([this] { try { engine_->stop(); } catch (...) {} });
        started_ = false;
    }
    try {
        engine_->deactivate();
    } catch (...) {
    }
    return initialise(rate_, max_block_);
}

expected<void, HostError> Renderer::reset() { return restart_engine(); }

expected<void, HostError> Renderer::open_editor() { return guarded("open_editor", [&] { return engine_->open_editor(); }); }

void Renderer::close_editor() {
    try {
        engine_->close_editor();
    } catch (...) {
    }
}

expected<std::vector<PresetRef>, HostError> Renderer::presets() {
    std::vector<PresetRef> out;
    try {
        out = engine_->presets();
    } catch (...) {
        return unexpected(HostError{"presets: plugin threw"});
    }
    // Program parameters (the heuristic step of the ladder, 06 section 6), for both formats alike: a stepped
    // program-change parameter that no program list already drives, one preset per value.
    for (const ParamDesc& p : engine_->parameters()) {
        if (!p.flags.program_change || p.steps < 2 || p.flags.readonly) continue;
        const bool listed = std::ranges::any_of(out, [&](const PresetRef& r) {
            return r.method == PresetMethod::ProgramList && r.param_id == p.id;
        });
        if (listed) continue;
        for (int i = 0; i < p.steps; ++i) {
            PresetRef r;
            r.method = PresetMethod::ProgramParam;
            r.param_id = p.id;
            r.index = i;
            r.count = p.steps;
            r.name = value_text(p.id, static_cast<double>(i) / (p.steps - 1));
            out.push_back(std::move(r));
        }
    }
    return out;
}

expected<void, HostError> Renderer::run_silence(int frames, std::span<const ParamValue> first, int program, int bank) {
    std::vector<float> left(static_cast<std::size_t>(max_block_)), right(static_cast<std::size_t>(max_block_));
    int done = 0;
    bool head = true;
    while (head || done < frames) {
        const int n = std::min(max_block_, std::max(frames - done, 1));
        Block b{n, head ? first : std::span<const ParamValue>{}, {}, head ? program : -1, head ? bank : -1};
        expected<void, HostError> r;
        thread_.run([&] { r = guarded("process", [&] { return engine_->process(b, left.data(), right.data()); }); });
        if (!r) return r;
        head = false;
        done += n;
    }
    return {};
}

expected<CapturedPreset, HostError> Renderer::apply_preset(const PresetRef& ref, double wait_ms) {
    message_.clear();
    // From the clean state, so a preset never inherits what the previous one set.
    if (auto r = restart_engine(); !r) return unexpected(r.error());
    if (!clean_.empty())
        if (auto r = load_state(clean_); !r) return unexpected(r.error());
    const auto& params = engine_->parameters();
    const int wait_frames = std::max(frames_for(wait_ms, rate_), 1);

    auto param_index = [&](std::uint32_t id) -> std::optional<std::size_t> {
        for (std::size_t i = 0; i < params.size(); ++i)
            if (params[i].id == id) return i;
        return std::nullopt;
    };

    expected<void, HostError> loaded;
    switch (ref.method) {
        case PresetMethod::File:
        case PresetMethod::ClapPreset:
        case PresetMethod::ProgramData: {
            loaded = guarded("load_preset", [&] { return engine_->load_preset(ref); });
            if (!loaded) break;
            // An asynchronous load (CLAP preset-load) reports `loaded` or `on_error` on the main thread. A plugin
            // need not report at all, so after the wait the load is taken as done [D]. The wait is 10 × wait_ms
            // until a load ends unreported; from then on only wait_ms, until the plugin reports again [D].
            const double factor = preset_unreported_ ? 1.0 : 10.0;
            const auto until = std::chrono::steady_clock::now() + std::chrono::duration<double, std::milli>(std::max(wait_ms, 1.0) * factor);
            for (;;) {
                pump();
                const PresetLoadStatus st = engine_->preset_status();
                if (!st.error.empty()) {
                    loaded = unexpected(HostError{"the plugin reported a load error: " + st.error});
                    preset_unreported_ = false;
                    break;
                }
                if (!st.pending) {
                    preset_unreported_ = false;
                    break;
                }
                if (std::chrono::steady_clock::now() >= until) {
                    preset_unreported_ = true;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            if (loaded) loaded = run_silence(wait_frames, {}, -1, -1);
            break;
        }
        case PresetMethod::ProgramList:
        case PresetMethod::ProgramParam: {
            if (!param_index(ref.param_id) || ref.count < 1) {
                loaded = unexpected(HostError{std::format("no program parameter {}", ref.param_id)});
                break;
            }
            // The one place a program-change parameter is sent (spec 05 section 9).
            const double u = ref.count > 1 ? static_cast<double>(ref.index) / (ref.count - 1) : 0.0;
            const ParamValue pv{ref.param_id, std::clamp(u, 0.0, 1.0)};
            try {
                engine_->apply_controller_values(std::span<const ParamValue>(&pv, 1));
            } catch (...) {
            }
            loaded = run_silence(wait_frames, std::span<const ParamValue>(&pv, 1), -1, -1);
            break;
        }
        case PresetMethod::MidiProgram:
            if (!engine_->midi_program_change()) {
                loaded = unexpected(HostError{"the plugin takes no MIDI program change"});
                break;
            }
            loaded = run_silence(wait_frames, {}, std::clamp(ref.index, 0, 127), ref.bank);
            break;
    }
    if (loaded) loaded = handle_restart_request();
    if (!loaded) {
        recover();
        return unexpected(loaded.error());
    }

    CapturedPreset out;
    out.name = ref.name;
    auto st = save_state();
    if (!st) {
        recover();
        return unexpected(st.error());
    }
    out.state = std::move(*st);
    // Loading the blob back proves it loads on its own and brings a separate controller (VST3) in line, so the
    // values read next are the ones the state holds.
    if (auto r = load_state(out.state); !r) {
        recover();
        return unexpected(HostError{"the captured state does not load back: " + r.error().message});
    }
    pump();
    out.coords.reserve(params.size());
    for (const ParamDesc& p : params) {
        std::optional<double> u;
        try {
            u = engine_->param_coord(p.id);
        } catch (...) {
        }
        out.coords.push_back(u && std::isfinite(*u) ? std::clamp(*u, 0.0, 1.0) : p.default_coord);
    }
    // Back to the clean state: later renders must not inherit the preset's hidden state.
    if (auto r = restart_engine(); r && !clean_.empty()) (void)load_state(clean_);
    return out;
}

Outcome Renderer::fail(Outcome outcome, std::string message) {
    message_ = std::move(message);
    return outcome;
}

expected<Audio, Outcome> Renderer::render(const Job& job) {
    auto r = render_once(job);
    if (!r) {
        const std::string why = message_;
        recover();
        message_ = why;
    }
    // Restart requests are not failures: re-enumerate, bump the version (spec 05 section 9). Checked
    // after failed renders too, so a table change is never missed.
    if (auto h = handle_restart_request(); !h && r) return unexpected(fail(Outcome::HostError, h.error().message));
    return r;
}

expected<Audio, Outcome> Renderer::render_once(const Job& job) {
    message_.clear();
    const auto& params = engine_->parameters();
    const bool from_state = !job.state.empty();
    if (!from_state && job.coords.size() != params.size())
        return unexpected(fail(Outcome::HostError, std::format("job has {} coordinates, plugin has {} parameters",
                                                              job.coords.size(), params.size())));
    if (job.sample_rate != rate_ || job.block_size > max_block_ || job.block_size < 1) {
        // A different rate or a larger block needs a new activation.
        rate_ = job.sample_rate;
        max_block_ = std::max(job.block_size, 1);
        if (auto r = restart_engine(); !r) return unexpected(fail(Outcome::HostError, r.error().message));
    } else if (reset_each_render_ || from_state) {
        if (auto r = restart_engine(); !r) return unexpected(fail(Outcome::HostError, r.error().message));
    }
    // A source render of a harvested preset: the complete state replaces the vector (spec 02 section 7).
    if (from_state)
        if (auto r = load_state(job.state); !r) return unexpected(fail(Outcome::HostError, r.error().message));

    // The full vector. Read-only and program-change parameters are never sent: sending even its default IS a
    // program change (a wrapper turns it into a MIDI program change that may land after the values and
    // overwrite them), so a program parameter is pinned by leaving it alone (spec 05 section 9). Bypass is
    // pinned to its default.
    std::vector<ParamValue> values;
    values.reserve(params.size());
    for (std::size_t i = 0; i < params.size() && !from_state; ++i) {
        const ParamDesc& p = params[i];
        if (p.flags.readonly || p.flags.program_change) continue;
        const double u = p.flags.bypass ? p.default_coord : job.coords[i];
        values.push_back({p.id, std::clamp(std::isfinite(u) ? u : p.default_coord, 0.0, 1.0)});
    }

    try {
        engine_->apply_controller_values(values);
    } catch (...) {
    }

    // Note plan to sample-exact events, relative to the first note (after the settle period).
    const int rate = rate_;
    const int block = std::max(1, job.block_size);
    const int settle = std::max(frames_for(job.settle_ms, rate), 0);
    std::vector<NoteEvent> events;
    std::int32_t next_id = 1;
    double plan_end_ms = 0;
    for (const Note& n : job.notes) {
        const std::int32_t id = next_id++;
        NoteEvent on{static_cast<std::uint32_t>(std::max(frames_for(n.on_ms, rate), 0)), true, n.key, n.cents, n.velocity, id};
        if (job.bend_range > 0) {  // cents as pitch bend (spec 02 section 5)
            on.set_bend = true;
            on.bend = std::clamp(n.cents / (100.0 * job.bend_range), -1.0, 1.0);
            on.cents = 0;
        }
        events.push_back(on);
        // The off carries the on's cents: a backend that rounds cents to a key must release the same key.
        events.push_back({static_cast<std::uint32_t>(std::max(frames_for(std::max(n.off_ms, n.on_ms), rate), 0)),
                          false, n.key, on.cents, 0.0f, id});
        plan_end_ms = std::max(plan_end_ms, std::max(n.off_ms, n.on_ms));
    }
    // Note-off before note-on at the same frame would release a note that has not started: keep the
    // order stable by frame, with offs after ons only when they belong to the same note (already so).
    std::stable_sort(events.begin(), events.end(),
                     [](const NoteEvent& a, const NoteEvent& b) { return a.frame < b.frame; });

    const std::int64_t plan_end = frames_for(plan_end_ms, rate);
    const std::int64_t tail_max = std::max(frames_for(job.tail_max_ms, rate), 0);
    // At least one frame past the last event, so an event at plan_end is delivered even without a tail.
    const std::int64_t last_event = events.empty() ? 0 : static_cast<std::int64_t>(events.back().frame) + 1;
    const std::int64_t hard_end = std::max(plan_end + tail_max, last_event);
    const int quiet_needed = frames_for(kQuietSeconds * 1000.0, rate);
    const double quiet_linear = std::pow(10.0, kQuietDb / 20.0);

    Audio out;
    out.sample_rate = rate;
    out.channels = 2;
    out.samples.reserve(static_cast<std::size_t>(hard_end > 0 ? std::min<std::int64_t>(hard_end, 60LL * rate) : 0) * 2);

    std::vector<float> left(static_cast<std::size_t>(block)), right(static_cast<std::size_t>(block));
    std::vector<NoteEvent> block_events;
    Outcome failed = Outcome::Ok;
    std::string failure;
    double peak = 0;
    std::int64_t quiet_run = 0;

    // One call to the engine on the render thread, with the exception guard and the per-block checks.
    // Returns false on a failed check (failed / failure set).
    const auto run_block = [&](int frames, std::span<const ParamValue> pv, std::span<const NoteEvent> ev,
                               bool record) -> bool {
        expected<void, HostError> r;
        if (blocks_) std::atomic_ref<std::uint64_t>(*blocks_).fetch_add(1, std::memory_order_relaxed);
        thread_.run([&] {
            r = guarded("process", [&] {
                return engine_->process(Block{frames, pv, ev}, left.data(), right.data());
            });
        });
        if (!r) {
            failed = Outcome::HostError;
            failure = r.error().message;
            return false;
        }
        double block_peak = 0;
        for (int i = 0; i < frames; ++i) {
            const float l = left[static_cast<std::size_t>(i)], rr = right[static_cast<std::size_t>(i)];
            if (!std::isfinite(l) || !std::isfinite(rr)) {
                failed = Outcome::NonFinite;
                failure = "non-finite sample in the output";
                return false;
            }
            block_peak = std::max({block_peak, static_cast<double>(std::fabs(l)), static_cast<double>(std::fabs(rr))});
        }
        if (block_peak > job.peak_limit) {
            failed = Outcome::Overload;
            failure = std::format("peak {:.1f} above the limit {:.1f}", block_peak, job.peak_limit);
            return false;
        }
        peak = std::max(peak, block_peak);
        if (record) {
            for (int i = 0; i < frames; ++i) {
                out.samples.push_back(left[static_cast<std::size_t>(i)]);
                out.samples.push_back(right[static_cast<std::size_t>(i)]);
            }
            quiet_run = block_peak < quiet_linear ? quiet_run + frames : 0;
        }
        return true;
    };

    // Block 0 carries every parameter at time 0; then silence until the settle time has passed.
    // Output during the settle period is checked but not returned.
    {
        int done = 0;
        bool first = true;
        while (first || done < settle) {
            const int n = std::min(block, std::max(settle - done, 1));
            if (!run_block(n, first ? std::span<const ParamValue>(values) : std::span<const ParamValue>{}, {}, false))
                return unexpected(fail(failed, failure));
            first = false;
            done += n;
        }
    }
    peak = 0;  // the silence check is on the returned part only

    // Notes, then the tail.
    std::int64_t pos = 0;
    std::size_t next_event = 0;
    while (pos < hard_end) {
        const int n = static_cast<int>(std::min<std::int64_t>(block, hard_end - pos));
        block_events.clear();
        while (next_event < events.size() && static_cast<std::int64_t>(events[next_event].frame) < pos + n) {
            NoteEvent e = events[next_event++];
            e.frame = static_cast<std::uint32_t>(std::max<std::int64_t>(e.frame - pos, 0));
            block_events.push_back(e);
        }
        if (!run_block(n, {}, block_events, true)) return unexpected(fail(failed, failure));
        pos += n;
        if (pos >= plan_end && next_event >= events.size() && quiet_run >= quiet_needed) break;
    }

    // Checks on the whole render.
    if (!out.samples.empty()) {
        // Runaway DC: |mean| above half the peak over the last second.
        const std::size_t tail_frames = std::min<std::size_t>(out.samples.size() / 2, static_cast<std::size_t>(rate));
        double sum = 0;
        for (std::size_t i = out.samples.size() - tail_frames * 2; i < out.samples.size(); i += 2)
            sum += 0.5 * (static_cast<double>(out.samples[i]) + static_cast<double>(out.samples[i + 1]));
        const double mean = tail_frames ? sum / static_cast<double>(tail_frames) : 0.0;
        if (peak > 1e-3 && std::fabs(mean) > 0.5 * peak)
            return unexpected(fail(Outcome::DcRunaway, std::format("DC {:.3f} against peak {:.3f}", mean, peak)));
    }
    if (!job.allow_silent && peak < std::pow(10.0, kSilenceDb / 20.0))
        return unexpected(fail(Outcome::Silent, "output below -90 dBFS"));
    return out;
}

}  // namespace mimicry::host
