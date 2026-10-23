#include "targets/Tones.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <fstream>
#include <sstream>

#include "core/Json.hpp"
#include "opt/Objective.hpp"

namespace mimicry::targets {

namespace {

constexpr double kWindowMs = 10.0;

double pair_distance(const opt::Embedding& a, const opt::Embedding& b) {
    const double level = a.size() > opt::kEmbeddingLevel && b.size() > opt::kEmbeddingLevel
                             ? std::fabs(double(a[opt::kEmbeddingLevel]) - double(b[opt::kEmbeddingLevel]))
                             : 0.0;
    return std::max(opt::embedding_distance(a, b), level);
}

}  // namespace

Consensus pick_consensus(const std::vector<opt::Embedding>& embeddings, const std::vector<bool>& valid) {
    Consensus c;
    const std::size_t n = embeddings.size();
    c.mean_distance.assign(n, 0.0);
    std::vector<std::size_t> idx;
    for (std::size_t i = 0; i < n; ++i)
        if (i < valid.size() && valid[i]) idx.push_back(i);
    if (idx.empty()) return c;
    if (idx.size() == 1) {
        c.index = static_cast<int>(idx[0]);
        return c;
    }
    double best = 0;
    for (const std::size_t i : idx) {
        double sum = 0;
        for (const std::size_t j : idx)
            if (j != i) sum += pair_distance(embeddings[i], embeddings[j]);
        c.mean_distance[i] = sum / double(idx.size() - 1);
        if (c.index < 0 || sum < best) {
            best = sum;
            c.index = static_cast<int>(i);
        }
    }
    c.spread = c.mean_distance[static_cast<std::size_t>(c.index)];
    return c;
}

std::size_t note_end_frame(const host::Audio& audio, double end_db) {
    if (audio.channels < 1 || audio.sample_rate < 1) return 0;
    const std::size_t ch = static_cast<std::size_t>(audio.channels);
    const std::size_t frames = audio.samples.size() / ch;
    const std::size_t win = std::max<std::size_t>(1, static_cast<std::size_t>(kWindowMs * 1e-3 * audio.sample_rate));
    const double limit = std::pow(10.0, end_db / 20.0);
    std::size_t end = 0;
    for (std::size_t start = 0; start < frames; start += win) {
        const std::size_t stop = std::min(frames, start + win);
        double peak = 0;
        for (std::size_t f = start; f < stop; ++f)
            for (std::size_t c = 0; c < ch; ++c) peak = std::max(peak, double(std::fabs(audio.samples[f * ch + c])));
        if (peak >= limit) end = stop;
    }
    return end;
}

expected<ToneResult, std::string> render_tone(host::Instance& inst, const std::vector<std::uint8_t>& state,
                                              const ToneOptions& o) {
    if (state.empty()) return unexpected(std::string("empty preset state"));
    if (o.takes < 1) return unexpected(std::string("at least one take is needed"));
    host::Job job;
    job.state = state;
    job.sample_rate = o.sample_rate;
    job.settle_ms = o.settle_ms;
    job.tail_max_ms = o.max_tail_ms;
    job.notes.push_back({o.key, 0, static_cast<float>(std::clamp(o.velocity, 1, 127)) / 127.0f, 0, o.gate_ms});

    ToneResult r;
    opt::Embedder embedder(o.sample_rate);
    std::vector<opt::Embedding> embs;
    std::vector<bool> valid;
    for (int t = 0; t < o.takes; ++t) {
        Take take;
        auto audio = inst.render(job);  // each take: reset, then the complete state (rule 5)
        if (!audio) {
            take.why = std::format("{}{}", to_string(audio.error()), inst.last_message().empty() ? "" : ": " + inst.last_message());
        } else {
            const std::size_t keep = note_end_frame(*audio, o.end_db);
            if (keep == 0) {
                take.why = "silent";
            } else {
                const std::size_t ch = static_cast<std::size_t>(audio->channels);
                // Never shorter than the held note, so a quick decay still keeps the attack and sustain.
                const std::size_t gate = static_cast<std::size_t>(o.gate_ms * 1e-3 * audio->sample_rate);
                const std::size_t frames = std::min(audio->samples.size() / ch, std::max(keep, gate));
                audio->samples.resize(frames * ch);
                const std::size_t fade = std::min(frames, static_cast<std::size_t>(o.fade_ms * 1e-3 * audio->sample_rate));
                for (std::size_t i = 0; i < fade; ++i) {
                    const float g = static_cast<float>(fade - i) / static_cast<float>(fade);
                    for (std::size_t c = 0; c < ch; ++c) audio->samples[(frames - fade + i) * ch + c] *= g;
                }
                take.ok = true;
                take.audio = std::move(*audio);
            }
        }
        embs.push_back(take.ok ? embedder.embed(opt::mono_of(take.audio)) : opt::Embedding{});
        take.level_db = take.ok && embs.back().size() > opt::kEmbeddingLevel ? embs.back()[opt::kEmbeddingLevel] : 0.0;
        valid.push_back(take.ok);
        r.valid += take.ok ? 1 : 0;
        r.takes.push_back(std::move(take));
    }
    const Consensus c = pick_consensus(embs, valid);
    for (std::size_t i = 0; i < r.takes.size(); ++i) r.takes[i].distance = c.mean_distance[i];
    r.chosen = c.index;
    r.spread = c.spread;
    r.unstable = c.index >= 0 && c.spread > o.max_spread;
    return r;
}

expected<std::vector<LibraryPreset>, std::string> list_library(const std::filesystem::path& dir) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return unexpected(dir.string() + " is not a folder");
    std::vector<LibraryPreset> out;
    if (std::ifstream in(dir / "index.json", std::ios::binary); in) {
        std::stringstream ss;
        ss << in.rdbuf();
        auto v = json::parse(ss.str());
        if (!v) return unexpected("cannot read " + (dir / "index.json").string() + ": " + v.error());
        for (const auto& e : (*v)["presets"].as_array()) {
            const std::string state = e["state"].as_string();
            if (e["outcome"].as_string() != "ok" || state.empty()) continue;
            LibraryPreset p;
            p.state = dir / state;
            p.stem = p.state.stem().string();
            p.name = e["name"].as_string().empty() ? p.stem : e["name"].as_string();
            if (const std::string pj = e["params"].as_string(); !pj.empty()) p.params = dir / pj;
            out.push_back(std::move(p));
        }
    } else {
        for (const auto& f : fs::directory_iterator(dir, ec)) {
            if (!f.is_regular_file() || f.path().extension() != ".state") continue;
            LibraryPreset p;
            p.state = f.path();
            p.stem = p.name = f.path().stem().string();
            if (auto pj = fs::path(f.path()).replace_extension(".params.json"); fs::exists(pj)) p.params = pj;
            out.push_back(std::move(p));
        }
        std::ranges::sort(out, {}, &LibraryPreset::stem);
    }
    if (out.empty()) return unexpected("no presets (index.json with ok presets, or .state files) in " + dir.string());
    return out;
}

std::string tone_stem(int key, int velocity) {
    static constexpr const char* names[] = {"C", "Cs", "D", "Ds", "E", "F", "Fs", "G", "Gs", "A", "As", "B"};
    return std::format("{}{}_v{}", names[((key % 12) + 12) % 12], key / 12 - 1, velocity);
}

}  // namespace mimicry::targets
