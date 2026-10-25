#include "output/VstPreset.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace mimicry::output {

namespace {

void put(std::vector<std::uint8_t>& out, std::uint64_t v, int bytes) {
    for (int i = 0; i < bytes; ++i) out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}

std::uint64_t get(const std::vector<std::uint8_t>& b, std::size_t at, int bytes) {
    std::uint64_t v = 0;
    for (int i = 0; i < bytes; ++i) v |= static_cast<std::uint64_t>(b[at + static_cast<std::size_t>(i)]) << (8 * i);
    return v;
}

constexpr std::size_t kHeader = 4 + 4 + 32 + 8;

}  // namespace

expected<VstPresetChunks, std::string> split_vst3_state(const std::vector<std::uint8_t>& blob) {
    VstPresetChunks out;
    std::size_t pos = 0;
    for (auto* part : {&out.component, &out.controller}) {
        if (pos + 4 > blob.size()) return unexpected(std::string("the VST3 state blob is malformed"));
        const auto n = static_cast<std::size_t>(get(blob, pos, 4));
        pos += 4;
        if (n > blob.size() - pos) return unexpected(std::string("the VST3 state blob is malformed"));
        part->assign(blob.begin() + static_cast<std::ptrdiff_t>(pos), blob.begin() + static_cast<std::ptrdiff_t>(pos + n));
        pos += n;
    }
    return out;
}

std::vector<std::uint8_t> vst3_state_from_chunks(const VstPresetChunks& chunks) {
    std::vector<std::uint8_t> blob;
    for (const auto* part : {&chunks.component, &chunks.controller}) {
        put(blob, part->size(), 4);
        blob.insert(blob.end(), part->begin(), part->end());
    }
    return blob;
}

expected<std::vector<std::uint8_t>, std::string> vstpreset_bytes(const std::string& class_id, const VstPresetChunks& chunks) {
    if (class_id.size() != 32 || !std::all_of(class_id.begin(), class_id.end(), [](unsigned char c) { return std::isxdigit(c) != 0; }))
        return unexpected("a VST3 class id is 32 hex characters, got '" + class_id + "'");
    std::string id = class_id;
    for (auto& c : id) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    std::vector<std::uint8_t> out;
    out.insert(out.end(), {'V', 'S', 'T', '3'});
    put(out, 1, 4);
    out.insert(out.end(), id.begin(), id.end());
    put(out, 0, 8);  // the list offset, patched below
    struct Entry {
        const char* id;
        std::uint64_t offset, size;
    };
    std::vector<Entry> entries;
    entries.push_back({"Comp", out.size(), chunks.component.size()});
    out.insert(out.end(), chunks.component.begin(), chunks.component.end());
    if (!chunks.controller.empty()) {
        entries.push_back({"Cont", out.size(), chunks.controller.size()});
        out.insert(out.end(), chunks.controller.begin(), chunks.controller.end());
    }
    const std::uint64_t list = out.size();
    out.insert(out.end(), {'L', 'i', 's', 't'});
    put(out, entries.size(), 4);
    for (const auto& e : entries) {
        out.insert(out.end(), e.id, e.id + 4);
        put(out, e.offset, 8);
        put(out, e.size, 8);
    }
    for (int i = 0; i < 8; ++i) out[40 + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(list >> (8 * i));
    return out;
}

expected<std::vector<std::uint8_t>, std::string> vstpreset_from_state(const std::string& class_id, const std::vector<std::uint8_t>& state) {
    auto parts = split_vst3_state(state);
    if (!parts) return unexpected(parts.error());
    return vstpreset_bytes(class_id, *parts);
}

expected<VstPresetFile, std::string> parse_vstpreset(const std::vector<std::uint8_t>& b) {
    if (b.size() < kHeader || std::memcmp(b.data(), "VST3", 4) != 0) return unexpected(std::string("not a .vstpreset file"));
    VstPresetFile f;
    f.class_id.assign(reinterpret_cast<const char*>(b.data() + 8), 32);
    const std::uint64_t list = get(b, 40, 8);
    if (list > b.size() || b.size() - list < 8 || std::memcmp(b.data() + list, "List", 4) != 0)
        return unexpected(std::string("bad chunk list"));
    const auto count = static_cast<std::size_t>(get(b, static_cast<std::size_t>(list) + 4, 4));
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t e = static_cast<std::size_t>(list) + 8 + i * 20;
        if (e + 20 > b.size()) return unexpected(std::string("bad chunk list"));
        const std::uint64_t off = get(b, e + 4, 8), size = get(b, e + 12, 8);
        if (off > b.size() || size > b.size() - off) return unexpected(std::string("chunk outside the file"));
        std::vector<std::uint8_t> chunk(b.begin() + static_cast<std::ptrdiff_t>(off), b.begin() + static_cast<std::ptrdiff_t>(off + size));
        if (std::memcmp(b.data() + e, "Comp", 4) == 0) f.chunks.component = std::move(chunk);
        else if (std::memcmp(b.data() + e, "Cont", 4) == 0) f.chunks.controller = std::move(chunk);
    }
    return f;
}

}  // namespace mimicry::output
