#include "opt/Objective.hpp"

#include <algorithm>
#include <thread>

#include "features/Preprocess.hpp"

namespace mimicry::opt {

std::vector<float> mono_of(const host::Audio& audio) {
    return features::mix_to_mono(audio.samples, std::max(1, audio.channels));
}

AudioObjective::AudioObjective(RenderBatch render, host::Job job, const features::Target& target,
                               features::Weights weights, features::ScorerOptions options, int threads,
                               std::size_t cache_bytes)
    : render_(std::move(render)), job_(std::move(job)), target_(target), cache_(cache_bytes) {
    threads = std::max(1, threads);
    for (int i = 0; i < threads; ++i) {
        scorers_.push_back(std::make_unique<features::Scorer>(target, weights, options));
        embedders_.push_back(std::make_unique<Embedder>(job_.sample_rate));
    }
}

double AudioObjective::seconds_per_candidate() const {
    const long n = candidates_.load();
    return n > 0 ? double(nanos_.load()) * 1e-9 / double(n) : 0.0;
}

long AudioObjective::offset() const { return scorers_.front()->offset(); }

std::shared_ptr<const host::Audio> AudioObjective::render(const std::vector<double>& coords, Outcome* outcome) {
    host::Job j = job_;
    j.coords = coords;
    const std::string key = RenderCache::key(j);
    auto hit = cache_.find(key);
    if (!hit) {
        auto r = render_({j});
        CachedRender c;
        if (!r.empty() && r[0]) {
            c.audio = std::make_shared<const host::Audio>(std::move(*r[0]));
        } else {
            c.outcome = r.empty() ? Outcome::HostError : r[0].error();
        }
        cache_.insert(key, c);
        hit = c;
    }
    if (outcome) *outcome = hit->outcome;
    return hit->outcome == Outcome::Ok ? hit->audio : nullptr;
}

bool AudioObjective::calibrate_offset(const std::vector<double>& coords) {
    const auto audio = render(coords);
    if (!audio) return false;
    const long off = scorers_.front()->estimate_offset(mono_of(*audio));
    for (auto& s : scorers_) s->set_offset(off);
    return true;
}

std::vector<Evaluation> AudioObjective::evaluate(const std::vector<std::vector<double>>& coords, bool embed) {
    const auto start = std::chrono::steady_clock::now();
    const std::size_t n = coords.size();
    std::vector<Evaluation> out(n);
    // Each candidate's renders (one, or `repeats_` for a noisy plugin).
    std::vector<std::vector<std::shared_ptr<const host::Audio>>> audio(n);
    std::vector<host::Job> jobs;
    std::vector<std::pair<std::size_t, std::string>> owners;  // candidate, cache key ("" = not cached)
    for (std::size_t i = 0; i < n; ++i) {
        host::Job j = job_;
        j.coords = coords[i];
        if (repeats_ == 1) {
            std::string key = RenderCache::key(j);
            if (auto hit = cache_.find(key)) {
                out[i].outcome = hit->outcome;
                out[i].message = hit->message;
                if (hit->audio) audio[i].push_back(hit->audio);
                continue;
            }
            jobs.push_back(std::move(j));
            owners.emplace_back(i, std::move(key));
        } else {
            for (int r = 0; r < repeats_; ++r) {
                jobs.push_back(j);
                owners.emplace_back(i, std::string());
            }
        }
    }
    if (!jobs.empty()) {
        auto results = render_(jobs);
        results.resize(jobs.size(), unexpected(Outcome::HostError));
        for (std::size_t k = 0; k < jobs.size(); ++k) {
            const auto [i, key] = owners[k];
            CachedRender c;
            if (results[k]) {
                c.audio = std::make_shared<const host::Audio>(std::move(*results[k]));
                audio[i].push_back(c.audio);
            } else {
                c.outcome = results[k].error();
                // The worst outcome of the repeats stands (a failure is information, rule 6).
                if (severity(c.outcome) > severity(out[i].outcome)) out[i].outcome = c.outcome;
            }
            if (!key.empty()) cache_.insert(key, std::move(c));
        }
    }

    // Score in parallel, one scorer per thread.
    std::atomic<std::size_t> next{0};
    auto work = [&](std::size_t t) {
        for (std::size_t i = next++; i < n; i = next++) {
            Evaluation& e = out[i];
            if (e.outcome != Outcome::Ok || audio[i].empty()) {
                if (e.outcome == Outcome::Ok) e.outcome = Outcome::HostError;
                continue;
            }
            double sum = 0;
            for (const auto& a : audio[i]) {
                const auto mono = mono_of(*a);
                const auto s = scorers_[t]->score(mono);
                sum += s.cost;
                e.score = s;  // the last render's breakdown
                if (embed && e.embedding.empty()) e.embedding = embedders_[t]->embed(mono);
            }
            e.loss = sum / double(audio[i].size());
            e.score.cost = e.loss;
        }
    };
    const std::size_t threads = std::min(scorers_.size(), n);
    std::vector<std::thread> pool;
    for (std::size_t t = 1; t < threads; ++t) pool.emplace_back(work, t);
    work(0);
    for (auto& t : pool) t.join();

    candidates_ += static_cast<long>(n);
    nanos_ += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
    return out;
}

}  // namespace mimicry::opt
