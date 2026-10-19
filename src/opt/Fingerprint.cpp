#include "opt/Fingerprint.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <fstream>
#include <numeric>
#include <thread>

#include "core/Json.hpp"
#include "core/Sampling.hpp"

namespace mimicry::opt {

namespace {
constexpr char kMagic[8] = {'M', 'I', 'M', 'F', 'P', 'D', 'B', '1'};
}

bool FingerprintDb::matches(const host::PluginInfo& info, const std::vector<host::ParamDesc>& params) const {
    if (info.format != format || info.uid != uid || info.version_hash != version_hash || params.size() != ids.size())
        return false;
    for (std::size_t i = 0; i < params.size(); ++i)
        if (params[i].id != ids[i] || params[i].name != names[i]) return false;
    return true;
}

std::vector<std::size_t> FingerprintDb::nearest(const Embedding& e, std::size_t k) const {
    std::vector<double> d(embeddings.size());
    for (std::size_t i = 0; i < embeddings.size(); ++i) d[i] = embedding_distance(e, embeddings[i]);
    std::vector<std::size_t> idx(d.size());
    std::iota(idx.begin(), idx.end(), 0);
    k = std::min(k, idx.size());
    std::partial_sort(idx.begin(), idx.begin() + long(k), idx.end(), [&](std::size_t a, std::size_t b) { return d[a] < d[b]; });
    idx.resize(k);
    return idx;
}

expected<void, std::string> FingerprintDb::save(const std::filesystem::path& path) const {
    std::error_code ec;
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream f(path, std::ios::binary);
    if (!f) return unexpected("cannot write " + path.string());
    json::Value h;
    h.set("format", std::string(host::to_string(format)));
    h.set("uid", uid);
    h.set("version_hash", version_hash);
    json::Array jid, jname;
    for (auto id : ids) jid.push_back(id);
    for (const auto& n : names) jname.push_back(n);
    h.set("ids", json::Value(std::move(jid)));
    h.set("names", json::Value(std::move(jname)));
    h.set("count", size());
    h.set("dims", kEmbeddingDims);
    h.set("rendered", static_cast<std::int64_t>(rendered));
    h.set("failed", static_cast<std::int64_t>(failed));
    f.write(kMagic, sizeof kMagic);
    f << h.dump() << '\n';
    for (std::size_t i = 0; i < size(); ++i) {
        f.write(reinterpret_cast<const char*>(coords[i].data()), std::streamsize(coords[i].size() * sizeof(float)));
        f.write(reinterpret_cast<const char*>(embeddings[i].data()), std::streamsize(kEmbeddingDims * sizeof(float)));
    }
    if (!f) return unexpected("cannot write " + path.string());
    return {};
}

expected<FingerprintDb, std::string> FingerprintDb::load(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return unexpected("cannot read " + path.string());
    char magic[8];
    if (!f.read(magic, 8) || std::memcmp(magic, kMagic, 8) != 0) return unexpected(path.string() + ": not a fingerprint database");
    std::string line;
    std::getline(f, line);
    auto h = json::parse(line);
    if (!h) return unexpected(path.string() + ": " + h.error());
    FingerprintDb db;
    db.format = (*h)["format"].as_string() == "VST3" ? host::PluginFormat::Vst3 : host::PluginFormat::Clap;
    db.uid = (*h)["uid"].as_string();
    db.version_hash = (*h)["version_hash"].as_string();
    for (const auto& v : (*h)["ids"].as_array()) db.ids.push_back(static_cast<std::uint32_t>(v.as_number()));
    for (const auto& v : (*h)["names"].as_array()) db.names.push_back(v.as_string());
    db.rendered = static_cast<long>((*h)["rendered"].as_number());
    db.failed = static_cast<long>((*h)["failed"].as_number());
    const auto count = static_cast<std::size_t>((*h)["count"].as_number());
    if (static_cast<std::size_t>((*h)["dims"].as_number()) != kEmbeddingDims || db.ids.size() != db.names.size())
        return unexpected(path.string() + ": incompatible fingerprint database");
    db.coords.resize(count, std::vector<float>(db.ids.size()));
    db.embeddings.resize(count, Embedding(kEmbeddingDims));
    for (std::size_t i = 0; i < count; ++i) {
        f.read(reinterpret_cast<char*>(db.coords[i].data()), std::streamsize(db.ids.size() * sizeof(float)));
        f.read(reinterpret_cast<char*>(db.embeddings[i].data()), std::streamsize(kEmbeddingDims * sizeof(float)));
    }
    if (!f) return unexpected(path.string() + ": truncated");
    return db;
}

std::filesystem::path FingerprintDb::default_path(const std::filesystem::path& dir, const host::PluginInfo& info) {
    std::string name = std::string(host::to_string(info.format)) + "-" + info.uid + "-" + info.version_hash;
    for (auto& c : name) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (!(std::isalnum(u) || c == '-' || c == '.' || c == '_')) c = '_';
    }
    return dir / (name + ".fingerprint");
}

FingerprintDb build_fingerprints(const RenderBatch& render, const host::PluginInfo& info, const Space& space,
                                 const FingerprintOptions& options) {
    FingerprintDb db;
    db.format = info.format;
    db.uid = info.uid;
    db.version_hash = info.version_hash;
    for (const auto& p : space.params()) {
        db.ids.push_back(p.id);
        db.names.push_back(p.name);
    }
    Rng rng(options.seed);
    const auto& active = space.active();
    const auto lhs = latin_hypercube(std::size_t(std::max(0L, options.count)), active.size(), rng);
    const std::size_t threads = std::size_t(std::max(1, options.threads));
    std::vector<std::unique_ptr<Embedder>> embedders;
    for (std::size_t t = 0; t < threads; ++t) embedders.push_back(std::make_unique<Embedder>(options.job.sample_rate));

    for (std::size_t start = 0; start < lhs.size(); start += std::size_t(std::max(1L, options.batch))) {
        const std::size_t end = std::min(lhs.size(), start + std::size_t(std::max(1L, options.batch)));
        std::vector<host::Job> jobs;
        for (std::size_t i = start; i < end; ++i) {
            host::Job j = options.job;
            j.coords = space.defaults();
            std::vector<double> x(active.size());
            for (std::size_t k = 0; k < active.size(); ++k) {
                const auto& s = space.slot(active[k]);
                x[k] = space.to_x(active[k], s.lo) + lhs[i][k] * (space.to_x(active[k], s.hi) - space.to_x(active[k], s.lo));
            }
            space.decode(active, x, j.coords);
            for (auto c : space.categorical()) {
                const auto vals = space.values(c);
                if (!vals.empty()) j.coords[c] = vals[rng.below(vals.size())];
            }
            jobs.push_back(std::move(j));
        }
        auto results = render(jobs);
        results.resize(jobs.size(), unexpected(Outcome::HostError));
        std::vector<Embedding> emb(jobs.size());
        std::atomic<std::size_t> next{0};
        auto work = [&](std::size_t t) {
            for (std::size_t i = next++; i < jobs.size(); i = next++)
                if (results[i]) emb[i] = embedders[t]->embed(mono_of(*results[i]));
        };
        std::vector<std::thread> pool;
        for (std::size_t t = 1; t < threads; ++t) pool.emplace_back(work, t);
        work(0);
        for (auto& t : pool) t.join();
        for (std::size_t i = 0; i < jobs.size(); ++i) {
            ++db.rendered;
            if (!results[i]) {
                ++db.failed;
                continue;
            }
            db.coords.emplace_back(jobs[i].coords.begin(), jobs[i].coords.end());
            db.embeddings.push_back(std::move(emb[i]));
        }
        if (options.progress) options.progress(long(end), long(lhs.size()));
    }
    return db;
}

}  // namespace mimicry::opt
