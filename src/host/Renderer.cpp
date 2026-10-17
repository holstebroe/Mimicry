#include "host/Renderer.hpp"

#include <algorithm>
#include <cmath>
#include <exception>
#include <format>

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
    return {};
}

std::string Renderer::value_text(std::uint32_t id, double coord) const { return engine_->value_text(id, coord); }

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

Outcome Renderer::fail(Outcome outcome, std::string message) {
    message_ = std::move(message);
    return outcome;
}

expected<Audio, Outcome> Renderer::render(const Job& job) {
    message_.clear();
    const auto& params = engine_->parameters();
    if (job.coords.size() != params.size())
        return unexpected(fail(Outcome::HostError, std::format("job has {} coordinates, plugin has {} parameters",
                                                              job.coords.size(), params.size())));
    if (job.sample_rate != rate_ || job.block_size > max_block_ || job.block_size < 1) {
        // A different rate or a larger block needs a new activation.
        rate_ = job.sample_rate;
        max_block_ = std::max(job.block_size, 1);
        if (auto r = restart_engine(); !r) return unexpected(fail(Outcome::HostError, r.error().message));
    } else if (reset_each_render_) {
        if (auto r = restart_engine(); !r) return unexpected(fail(Outcome::HostError, r.error().message));
    }

    // The full vector. Read-only parameters are not sent; bypass and program change are pinned to their
    // default; program change goes first so it cannot overwrite the values that follow.
    std::vector<ParamValue> values;
    values.reserve(params.size());
    for (std::size_t i = 0; i < params.size(); ++i)
        if (params[i].flags.program_change) values.push_back({params[i].id, params[i].default_coord});
    for (std::size_t i = 0; i < params.size(); ++i) {
        const ParamDesc& p = params[i];
        if (p.flags.readonly || p.flags.program_change) continue;
        const double u = p.flags.bypass ? p.default_coord : job.coords[i];
        values.push_back({p.id, std::clamp(std::isfinite(u) ? u : p.default_coord, 0.0, 1.0)});
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
        events.push_back({static_cast<std::uint32_t>(std::max(frames_for(n.on_ms, rate), 0)), true, n.key, n.cents,
                          n.velocity, id});
        events.push_back({static_cast<std::uint32_t>(std::max(frames_for(std::max(n.off_ms, n.on_ms), rate), 0)),
                          false, n.key, 0.0, 0.0f, id});
        plan_end_ms = std::max(plan_end_ms, std::max(n.off_ms, n.on_ms));
    }
    // Note-off before note-on at the same frame would release a note that has not started: keep the
    // order stable by frame, with offs after ons only when they belong to the same note (already so).
    std::stable_sort(events.begin(), events.end(),
                     [](const NoteEvent& a, const NoteEvent& b) { return a.frame < b.frame; });

    const std::int64_t plan_end = frames_for(plan_end_ms, rate);
    const std::int64_t tail_max = std::max(frames_for(job.tail_max_ms, rate), 0);
    const std::int64_t hard_end = plan_end + tail_max;
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

    // Restart requests are not failures: re-enumerate, bump the version (spec 05 section 9).
    if (engine_->take_restart_request()) {
        if (engine_->rescan_parameters()) ++version_;
        if (auto r = restart_engine(); !r) return unexpected(fail(Outcome::HostError, r.error().message));
    }
    return out;
}

}  // namespace mimicry::host
