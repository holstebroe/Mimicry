#pragma once
// `.vstpreset` writer and reader (docs/spec/07_OUTPUT_VALIDATION.md section 3) [S: VST3 SDK vstpresetfile.h]:
// "VST3", int32 version 1, the 32 ASCII characters of the class id, int64 offset of the chunk list; then the
// chunks ('Comp' component state, 'Cont' controller state) and the list ("List", int32 count, per entry a
// 4-character id, int64 offset, int64 size). All little-endian.
//
// Mimicry's VST3 state blob (Vst3Engine::save_state) is two length-prefixed parts, component then
// controller; the writer splits it into the two chunks. The host's own `.vstpreset` reader is the
// verification (T14).

#include <cstdint>
#include <string>
#include <vector>

#include "core/Expected.hpp"

namespace mimicry::output {

struct VstPresetChunks {
    std::vector<std::uint8_t> component, controller;
};

// Mimicry's VST3 state blob -> its two parts.
[[nodiscard]] expected<VstPresetChunks, std::string> split_vst3_state(const std::vector<std::uint8_t>& blob);

// `class_id` is the plugin's 32 hex characters (PluginInfo::uid of a VST3 plugin).
[[nodiscard]] expected<std::vector<std::uint8_t>, std::string> vstpreset_bytes(const std::string& class_id,
                                                                               const VstPresetChunks& chunks);
[[nodiscard]] expected<std::vector<std::uint8_t>, std::string> vstpreset_from_state(const std::string& class_id,
                                                                                    const std::vector<std::uint8_t>& state);

// The two chunks -> Mimicry's state blob (what Instance::load_state takes).
[[nodiscard]] std::vector<std::uint8_t> vst3_state_from_chunks(const VstPresetChunks& chunks);

// Reads a `.vstpreset` back: class id and chunks.
struct VstPresetFile {
    std::string class_id;
    VstPresetChunks chunks;
};
[[nodiscard]] expected<VstPresetFile, std::string> parse_vstpreset(const std::vector<std::uint8_t>& bytes);

}  // namespace mimicry::output
