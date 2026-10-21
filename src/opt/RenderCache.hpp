#pragma once
// Render cache (docs/spec/03_FEATURES_LOSS.md section 7): duplicate candidates render once [S: Acidus
// `_job_key`]. Keyed on the exact parameter bytes, note plan, rate, block size and the other render
// settings. Failed renders are cached too, since a candidate that crashed the plugin will crash it again
// (CLAUDE.md rule 6: failures are information); transient outcomes (timeout, host error) are not.
// Thread-safe. Header-only; it sees only the format-agnostic host interface.

#include <cstdint>
#include <cstring>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

#include "host/PluginHost.hpp"

namespace mimicry::opt {

struct CachedRender {
    Outcome outcome = Outcome::Ok;
    std::shared_ptr<const host::Audio> audio;  // set when outcome is Ok
    std::string message;
};

class RenderCache {
public:
    explicit RenderCache(std::size_t max_bytes = std::size_t{512} << 20) : max_bytes_(max_bytes) {}

    [[nodiscard]] static std::string key(const host::Job& job) {
        std::string k;
        auto put = [&k](const auto& v) { k.append(reinterpret_cast<const char*>(&v), sizeof v); };
        for (double c : job.coords) put(c);
        put(std::uint64_t(job.coords.size()));
        for (const auto& n : job.notes) {
            put(n.key);
            put(n.cents);
            put(n.velocity);
            put(n.on_ms);
            put(n.off_ms);
        }
        put(std::uint64_t(job.notes.size()));
        put(job.sample_rate);
        put(job.block_size);
        put(job.settle_ms);
        put(job.tail_max_ms);
        put(job.peak_limit);
        put(job.allow_silent);
        put(job.bend_range);
        // A state render keys on the whole blob: two presets may share every coordinate.
        put(std::uint64_t(job.state.size()));
        k.append(reinterpret_cast<const char*>(job.state.data()), job.state.size());
        return k;
    }

    [[nodiscard]] static bool cacheable(Outcome o) noexcept {
        return o != Outcome::Timeout && o != Outcome::HostError;
    }

    [[nodiscard]] std::optional<CachedRender> find(const std::string& key) {
        std::lock_guard lock(mutex_);
        const auto it = map_.find(key);
        if (it == map_.end()) {
            ++misses_;
            return std::nullopt;
        }
        ++hits_;
        order_.splice(order_.begin(), order_, it->second);  // most recently used first
        return it->second->value;
    }

    void insert(const std::string& key, CachedRender value) {
        if (!cacheable(value.outcome)) return;
        std::lock_guard lock(mutex_);
        if (map_.count(key)) return;
        const std::size_t bytes = key.size() + (value.audio ? value.audio->samples.size() * sizeof(float) : 0) + 64;
        order_.push_front({key, std::move(value), bytes});
        map_[key] = order_.begin();
        bytes_ += bytes;
        while (bytes_ > max_bytes_ && order_.size() > 1) {
            bytes_ -= order_.back().bytes;
            map_.erase(order_.back().key);
            order_.pop_back();
        }
    }

    // Renders through `render` (any callable job -> expected<Audio, Outcome>) unless the job is cached.
    // `message` is called after a failed render to fetch the reason (Instance::last_message).
    template <class RenderFn, class MessageFn>
    [[nodiscard]] CachedRender get_or_render(const host::Job& job, RenderFn&& render, MessageFn&& message) {
        const std::string k = key(job);
        if (auto hit = find(k)) return *hit;
        CachedRender out;
        auto r = render(job);
        if (r) {
            out.outcome = Outcome::Ok;
            out.audio = std::make_shared<const host::Audio>(std::move(*r));
        } else {
            out.outcome = r.error();
            out.message = message();
        }
        insert(k, out);
        return out;
    }

    [[nodiscard]] std::size_t hits() const { std::lock_guard l(mutex_); return hits_; }
    [[nodiscard]] std::size_t misses() const { std::lock_guard l(mutex_); return misses_; }
    [[nodiscard]] std::size_t size() const { std::lock_guard l(mutex_); return map_.size(); }
    [[nodiscard]] std::size_t bytes() const { std::lock_guard l(mutex_); return bytes_; }

private:
    struct Entry {
        std::string key;
        CachedRender value;
        std::size_t bytes;
    };
    mutable std::mutex mutex_;
    std::list<Entry> order_;
    std::unordered_map<std::string, std::list<Entry>::iterator> map_;
    std::size_t max_bytes_, bytes_ = 0, hits_ = 0, misses_ = 0;
};

}  // namespace mimicry::opt
