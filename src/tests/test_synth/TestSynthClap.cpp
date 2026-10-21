// mimicry_test_synth as a CLAP plugin (docs/spec/07_OUTPUT_VALIDATION.md section 5). The DSP, the parameter
// table and the planted hazards are in TestSynthCore.hpp, shared with the VST3 build.

#include <clap/clap.h>
#include <clap/ext/preset-load.h>
#include <clap/factory/preset-discovery.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <mutex>
#include <new>
#include <string>
#include <vector>

#include "TestSynthCore.hpp"

namespace {

using namespace mimicry::testsynth;

constexpr const char* kPluginId = "org.mimicry.test-synth";
const char* const kFeatures[] = {CLAP_PLUGIN_FEATURE_INSTRUMENT, CLAP_PLUGIN_FEATURE_SYNTHESIZER, nullptr};

const clap_plugin_descriptor_t kDescriptor = {
    CLAP_VERSION_INIT, kPluginId, "Mimicry Test Synth", "Mimicry", "", "", "",
    "0.1.0", "Test plugin with planted hazards", kFeatures,
};

struct Plugin {
    clap_plugin_t plugin{};
    const clap_host_t* host = nullptr;
    const clap_host_params_t* host_params = nullptr;
    const clap_host_preset_load_t* host_preset_load = nullptr;
    Synth synth;
    bool processing = false;
    // A preset-load request, run on the main thread from on_main_thread (an asynchronous load, as the CLAP
    // spec allows: the host must wait for `loaded`).
    std::mutex pending_mutex;
    bool pending = false;
    uint32_t pending_kind = 0;
    std::string pending_location, pending_key;
};

Plugin* self(const clap_plugin_t* p) { return static_cast<Plugin*>(p->plugin_data); }

// --- params ----------------------------------------------------------------------------------

uint32_t params_count(const clap_plugin_t*) { return kParamCount; }

bool params_get_info(const clap_plugin_t* plugin, uint32_t index, clap_param_info_t* out) {
    if (index >= kParamCount) return false;
    const ParamInfo& p = kParams[index];
    std::memset(out, 0, sizeof *out);
    out->id = p.id;
    out->flags = CLAP_PARAM_IS_AUTOMATABLE;
    if (p.stepped) out->flags |= CLAP_PARAM_IS_STEPPED;
    if (p.hidden) out->flags |= CLAP_PARAM_IS_HIDDEN;
    if (p.readonly) out->flags = CLAP_PARAM_IS_READONLY;
    if (p.bypass) out->flags |= CLAP_PARAM_IS_BYPASS;
    std::snprintf(out->name, sizeof out->name, "%s", self(plugin)->synth.name(index));
    std::snprintf(out->module, sizeof out->module, "%s", p.module);
    out->min_value = p.min;
    out->max_value = p.max;
    out->default_value = p.def;
    return true;
}

bool params_get_value(const clap_plugin_t* plugin, clap_id id, double* out) {
    if (id >= kParamCount) return false;
    *out = self(plugin)->synth.value(static_cast<ParamId>(id));
    return true;
}

bool params_value_to_text(const clap_plugin_t*, clap_id id, double value, char* out, uint32_t capacity) {
    if (id >= kParamCount) return false;
    const ParamInfo& p = kParams[id];
    if (id == kWave) {
        std::snprintf(out, capacity, "%s", value >= 0.5 ? "Saw" : "Sine");
    } else if (p.unit[0] == '\0') {
        std::snprintf(out, capacity, p.stepped ? "%.0f" : "%.3f", value);
    } else if (std::strcmp(p.unit, "%") == 0) {
        std::snprintf(out, capacity, "%.0f %%", value);
    } else {
        std::snprintf(out, capacity, "%.2f %s", value, p.unit);
    }
    return true;
}

bool params_text_to_value(const clap_plugin_t*, clap_id, const char* text, double* out) {
    char* end = nullptr;
    const double v = std::strtod(text, &end);
    if (end == text) return false;
    *out = v;
    return true;
}

void handle_event(Plugin& pl, const clap_event_header_t* h) {
    if (h->space_id != CLAP_CORE_EVENT_SPACE_ID) return;
    switch (h->type) {
        case CLAP_EVENT_PARAM_VALUE: {
            const auto* e = reinterpret_cast<const clap_event_param_value_t*>(h);
            pl.synth.set_param(e->param_id, e->value);
            break;
        }
        case CLAP_EVENT_NOTE_ON: {
            const auto* e = reinterpret_cast<const clap_event_note_t*>(h);
            pl.synth.note_on(e->key, e->velocity, e->note_id);
            break;
        }
        case CLAP_EVENT_NOTE_OFF:
        case CLAP_EVENT_NOTE_CHOKE: {
            const auto* e = reinterpret_cast<const clap_event_note_t*>(h);
            pl.synth.note_off(e->key, e->note_id);
            break;
        }
        case CLAP_EVENT_MIDI: {
            const auto* e = reinterpret_cast<const clap_event_midi_t*>(h);
            if ((e->data[0] & 0xF0) == 0xC0) pl.synth.load_factory(e->data[1]);  // the factory bank (T17)
            if ((e->data[0] & 0xF0) == 0xE0) {
                const int v = (int(e->data[2]) << 7) | int(e->data[1]);  // 0..16383, centre 8192
                pl.synth.pitch_bend(double(v - 8192) / 8192.0);
            }
            break;
        }
        case CLAP_EVENT_NOTE_EXPRESSION: {
            const auto* e = reinterpret_cast<const clap_event_note_expression_t*>(h);
            if (e->expression_id == CLAP_NOTE_EXPRESSION_TUNING) pl.synth.note_tuning(e->key, e->note_id, e->value);
            break;
        }
        default: break;
    }
}

void params_flush(const clap_plugin_t* plugin, const clap_input_events_t* in, const clap_output_events_t*) {
    Plugin& pl = *self(plugin);
    for (uint32_t i = 0; i < in->size(in); ++i) handle_event(pl, in->get(in, i));
}

const clap_plugin_params_t kParamsExt = {params_count,         params_get_info,     params_get_value,
                                         params_value_to_text, params_text_to_value, params_flush};

// --- audio and note ports --------------------------------------------------------------------

uint32_t audio_ports_count(const clap_plugin_t*, bool is_input) { return is_input ? 0 : 1; }

bool audio_ports_get(const clap_plugin_t*, uint32_t index, bool is_input, clap_audio_port_info_t* info) {
    if (is_input || index != 0) return false;
    info->id = 0;
    std::snprintf(info->name, sizeof info->name, "Main");
    info->flags = CLAP_AUDIO_PORT_IS_MAIN;
    info->channel_count = 2;
    info->port_type = CLAP_PORT_STEREO;
    info->in_place_pair = CLAP_INVALID_ID;
    return true;
}

const clap_plugin_audio_ports_t kAudioPorts = {audio_ports_count, audio_ports_get};

uint32_t note_ports_count(const clap_plugin_t*, bool is_input) { return is_input ? 1 : 0; }

bool note_ports_get(const clap_plugin_t*, uint32_t index, bool is_input, clap_note_port_info_t* info) {
    if (!is_input || index != 0) return false;
    info->id = 0;
    // MIDI too, for pitch bend (spec 02 section 5). The VST3 build (clap-wrapper) then maps MIDI controllers
    // through IMidiMapping and adds a MIDI program-change parameter.
    info->supported_dialects = CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI;
    info->preferred_dialect = CLAP_NOTE_DIALECT_CLAP;
    std::snprintf(info->name, sizeof info->name, "Notes");
    return true;
}

const clap_plugin_note_ports_t kNotePorts = {note_ports_count, note_ports_get};

// --- state, tail, latency --------------------------------------------------------------------

constexpr char kStateMagic[4] = {'M', 'T', 'S', '1'};

bool state_save(const clap_plugin_t* plugin, const clap_ostream_t* stream) {
    const Plugin& pl = *self(plugin);
    std::vector<char> buf(sizeof kStateMagic + kParamCount * sizeof(double));
    std::memcpy(buf.data(), kStateMagic, sizeof kStateMagic);
    for (uint32_t i = 0; i < kParamCount; ++i) {
        // The meter is output, not state: a saved state must not depend on the last block played.
        const double v = kParams[i].readonly ? kParams[i].def : pl.synth.value(static_cast<ParamId>(i));
        std::memcpy(buf.data() + sizeof kStateMagic + i * sizeof(double), &v, sizeof v);
    }
    int64_t done = 0;
    while (done < static_cast<int64_t>(buf.size())) {
        const int64_t n = stream->write(stream, buf.data() + done, buf.size() - static_cast<uint64_t>(done));
        if (n <= 0) return false;
        done += n;
    }
    return true;
}

bool state_from_bytes(Plugin& pl, const std::vector<char>& buf) {
    if (buf.size() != sizeof kStateMagic + kParamCount * sizeof(double)) return false;
    if (std::memcmp(buf.data(), kStateMagic, sizeof kStateMagic) != 0) return false;
    // The program parameter would overwrite wave and cutoff, so set it first, then the rest.
    double program;
    std::memcpy(&program, buf.data() + sizeof kStateMagic + kProgram * sizeof(double), sizeof program);
    pl.synth.set_param(kProgram, program);
    for (uint32_t i = 0; i < kParamCount; ++i) {
        if (i == kProgram || kParams[i].readonly) continue;
        double v;
        std::memcpy(&v, buf.data() + sizeof kStateMagic + i * sizeof(double), sizeof v);
        pl.synth.set_param(i, v);
    }
    return true;
}

bool state_load(const clap_plugin_t* plugin, const clap_istream_t* stream) {
    std::vector<char> buf(sizeof kStateMagic + kParamCount * sizeof(double));
    int64_t done = 0;
    while (done < static_cast<int64_t>(buf.size())) {
        const int64_t n = stream->read(stream, buf.data() + done, buf.size() - static_cast<uint64_t>(done));
        if (n <= 0) return false;
        done += n;
    }
    return state_from_bytes(*self(plugin), buf);
}

const clap_plugin_state_t kState = {state_save, state_load};

uint32_t tail_get(const clap_plugin_t* plugin) {
    return static_cast<uint32_t>(self(plugin)->synth.release_tail_seconds() * 48000.0);
}
const clap_plugin_tail_t kTail = {tail_get};

bool render_hard_realtime(const clap_plugin_t*) { return false; }
bool render_set(const clap_plugin_t* plugin, clap_plugin_render_mode mode) {
    self(plugin)->synth.set_offline(mode == CLAP_RENDER_OFFLINE);
    return true;
}
const clap_plugin_render_t kRender = {render_hard_realtime, render_set};

uint32_t latency_get(const clap_plugin_t*) { return 0; }
const clap_plugin_latency_t kLatency = {latency_get};

// --- preset-load (T17) -----------------------------------------------------------------------

// A preset file (MIMICRY_TEST_PRESET_DIR, extension .mtspreset) is a state blob.
constexpr const char* kPresetExtension = "mtspreset";

bool preset_from_location(const clap_plugin_t* plugin, uint32_t kind, const char* location, const char* key) {
    Plugin& pl = *self(plugin);
    if (kind != CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN && kind != CLAP_PRESET_DISCOVERY_LOCATION_FILE) return false;
    if (kind == CLAP_PRESET_DISCOVERY_LOCATION_FILE && !location) return false;
    {
        std::lock_guard lock(pl.pending_mutex);
        pl.pending = true;
        pl.pending_kind = kind;
        pl.pending_location = location ? location : "";
        pl.pending_key = key ? key : "";
    }
    if (pl.host && pl.host->request_callback) pl.host->request_callback(pl.host);
    return true;
}

const clap_plugin_preset_load_t kPresetLoad = {preset_from_location};

void run_pending_preset(Plugin& pl) {
    uint32_t kind;
    std::string location, key;
    {
        std::lock_guard lock(pl.pending_mutex);
        if (!pl.pending) return;
        pl.pending = false;
        kind = pl.pending_kind;
        location = pl.pending_location;
        key = pl.pending_key;
    }
    const char* loc = kind == CLAP_PRESET_DISCOVERY_LOCATION_FILE ? location.c_str() : nullptr;
    const char* k = key.empty() ? nullptr : key.c_str();
    bool ok = false;
    if (kind == CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN) {
        ok = k && pl.synth.load_factory(std::atoi(k));
    } else {
        std::ifstream f(location, std::ios::binary);
        const std::vector<char> buf((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        ok = f.good() || f.eof() ? state_from_bytes(pl, buf) : false;
    }
    if (!pl.host_preset_load) return;
    if (ok) pl.host_preset_load->loaded(pl.host, kind, loc, k);
    else pl.host_preset_load->on_error(pl.host, kind, loc, k, 0, "no such preset");
}

// --- plugin ----------------------------------------------------------------------------------

bool plugin_init(const clap_plugin_t* plugin) {
    Plugin& pl = *self(plugin);
    pl.synth.on_restart_request = [&pl] {
        if (pl.host && pl.host->request_restart) pl.host->request_restart(pl.host);
    };
    // A rescan belongs on the main thread; the hazard calls it from wherever the value arrives, which both
    // Mimicry's CLAP host and clap-wrapper handle synchronously.
    pl.host_params = pl.host ? static_cast<const clap_host_params_t*>(pl.host->get_extension(pl.host, CLAP_EXT_PARAMS)) : nullptr;
    pl.host_preset_load = pl.host ? static_cast<const clap_host_preset_load_t*>(pl.host->get_extension(pl.host, CLAP_EXT_PRESET_LOAD)) : nullptr;
    pl.synth.on_rename = [&pl] {
        if (pl.host_params && pl.host_params->rescan) pl.host_params->rescan(pl.host, CLAP_PARAM_RESCAN_INFO);
    };
    return true;
}

void plugin_destroy(const clap_plugin_t* plugin) { delete self(plugin); }

bool plugin_activate(const clap_plugin_t* plugin, double sample_rate, uint32_t, uint32_t) {
    self(plugin)->synth.activate(sample_rate);
    return true;
}

void plugin_deactivate(const clap_plugin_t*) {}

bool plugin_start_processing(const clap_plugin_t* plugin) {
    self(plugin)->processing = true;
    return true;
}

void plugin_stop_processing(const clap_plugin_t* plugin) { self(plugin)->processing = false; }

void plugin_reset(const clap_plugin_t*) {}

clap_process_status plugin_process(const clap_plugin_t* plugin, const clap_process_t* process) {
    Plugin& pl = *self(plugin);
    if (process->audio_outputs_count < 1 || process->audio_outputs[0].channel_count < 2) return CLAP_PROCESS_ERROR;
    float* left = process->audio_outputs[0].data32[0];
    float* right = process->audio_outputs[0].data32[1];
    const uint32_t frames = process->frames_count;
    const uint32_t events = process->in_events->size(process->in_events);

    uint32_t pos = 0, ev = 0;
    while (pos < frames) {
        while (ev < events) {
            const clap_event_header_t* h = process->in_events->get(process->in_events, ev);
            if (h->time > pos) break;
            handle_event(pl, h);
            ++ev;
        }
        uint32_t next = frames;
        if (ev < events) next = std::min<uint32_t>(next, process->in_events->get(process->in_events, ev)->time);
        pl.synth.process(left + pos, right + pos, static_cast<int>(next - pos));
        pos = next;
    }
    return CLAP_PROCESS_CONTINUE;
}

// The clap-wrapper's plugin extension (include/clapwrapper/vst3.h, "clap.plugin-info-as-vst3/0"), declared
// here to keep the CLAP build free of the wrapper: it tells the VST3 build to offer note tuning.
constexpr char kPluginAsVst3[] = "clap.plugin-info-as-vst3/0";
struct PluginAsVst3 {
    uint32_t (*num_midi_channels)(const clap_plugin_t*, uint32_t note_port);
    uint32_t (*supported_note_expressions)(const clap_plugin_t*);
};
constexpr uint32_t kVst3ExpressionTuning = 1u << 2;
const PluginAsVst3 kAsVst3 = {[](const clap_plugin_t*, uint32_t) -> uint32_t { return 1; },
                              [](const clap_plugin_t*) -> uint32_t { return kVst3ExpressionTuning; }};

const void* plugin_get_extension(const clap_plugin_t*, const char* id) {
    if (!std::strcmp(id, CLAP_EXT_PARAMS)) return &kParamsExt;
    if (!std::strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &kAudioPorts;
    if (!std::strcmp(id, CLAP_EXT_NOTE_PORTS)) return &kNotePorts;
    if (!std::strcmp(id, CLAP_EXT_STATE)) return &kState;
    if (!std::strcmp(id, CLAP_EXT_TAIL)) return &kTail;
    if (!std::strcmp(id, CLAP_EXT_LATENCY)) return &kLatency;
    if (!std::strcmp(id, CLAP_EXT_RENDER)) return &kRender;
    if (!std::strcmp(id, kPluginAsVst3)) return &kAsVst3;
    if (!std::strcmp(id, CLAP_EXT_PRESET_LOAD)) return &kPresetLoad;
    return nullptr;
}

void plugin_on_main_thread(const clap_plugin_t* plugin) { run_pending_preset(*self(plugin)); }

// --- factory ---------------------------------------------------------------------------------

uint32_t factory_count(const clap_plugin_factory_t*) { return 1; }

const clap_plugin_descriptor_t* factory_descriptor(const clap_plugin_factory_t*, uint32_t index) {
    return index == 0 ? &kDescriptor : nullptr;
}

const clap_plugin_t* factory_create(const clap_plugin_factory_t*, const clap_host_t* host, const char* id) {
    if (!id || std::strcmp(id, kPluginId) != 0) return nullptr;
    auto* pl = new (std::nothrow) Plugin;
    if (!pl) return nullptr;
    pl->host = host;
    pl->plugin = {&kDescriptor,        pl,
                  plugin_init,         plugin_destroy,    plugin_activate,    plugin_deactivate,
                  plugin_start_processing, plugin_stop_processing, plugin_reset, plugin_process,
                  plugin_get_extension, plugin_on_main_thread};
    return &pl->plugin;
}

const clap_plugin_factory_t kFactory = {factory_count, factory_descriptor, factory_create};

// --- preset discovery (T17) ------------------------------------------------------------------
// One provider: the factory bank as a plugin location (load key = index), and the folder in the environment
// variable MIMICRY_TEST_PRESET_DIR, if set, as a file location of .mtspreset files.

const clap_preset_discovery_provider_descriptor_t kProviderDesc = {CLAP_VERSION_INIT, "org.mimicry.test-synth.presets",
                                                                   "Mimicry Test Synth presets", "Mimicry"};

struct Provider {
    clap_preset_discovery_provider_t provider{};
    const clap_preset_discovery_indexer_t* indexer = nullptr;
};

bool provider_init(const clap_preset_discovery_provider_t* p) {
    const auto* ix = static_cast<Provider*>(p->provider_data)->indexer;
    const clap_preset_discovery_location_t factory{CLAP_PRESET_DISCOVERY_IS_FACTORY_CONTENT, "Factory",
                                                   CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr};
    ix->declare_location(ix, &factory);
    if (const char* dir = std::getenv("MIMICRY_TEST_PRESET_DIR"); dir && *dir) {
        const clap_preset_discovery_filetype_t ft{"Mimicry test preset", "", kPresetExtension};
        ix->declare_filetype(ix, &ft);
        const clap_preset_discovery_location_t user{CLAP_PRESET_DISCOVERY_IS_USER_CONTENT, "User",
                                                    CLAP_PRESET_DISCOVERY_LOCATION_FILE, dir};
        ix->declare_location(ix, &user);
    }
    return true;
}

void provider_destroy(const clap_preset_discovery_provider_t* p) { delete static_cast<Provider*>(p->provider_data); }

bool provider_get_metadata(const clap_preset_discovery_provider_t*, uint32_t kind, const char* location,
                           const clap_preset_discovery_metadata_receiver_t* r) {
    const clap_universal_plugin_id_t id{"clap", kPluginId};
    if (kind == CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN) {
        for (int i = 0; i < kFactoryCount; ++i) {
            const std::string key = std::to_string(i);
            if (!r->begin_preset(r, kFactoryBank[i].name, key.c_str())) break;
            r->add_plugin_id(r, &id);
            r->set_flags(r, CLAP_PRESET_DISCOVERY_IS_FACTORY_CONTENT);
        }
        return true;
    }
    if (!location) return false;
    // One preset per file: the host names it after the file when the name is null.
    if (r->begin_preset(r, nullptr, nullptr)) r->add_plugin_id(r, &id);
    return true;
}

const void* provider_get_extension(const clap_preset_discovery_provider_t*, const char*) { return nullptr; }

uint32_t discovery_count(const clap_preset_discovery_factory_t*) { return 1; }
const clap_preset_discovery_provider_descriptor_t* discovery_descriptor(const clap_preset_discovery_factory_t*, uint32_t i) {
    return i == 0 ? &kProviderDesc : nullptr;
}
const clap_preset_discovery_provider_t* discovery_create(const clap_preset_discovery_factory_t*,
                                                         const clap_preset_discovery_indexer_t* indexer, const char* id) {
    if (!id || std::strcmp(id, kProviderDesc.id) != 0) return nullptr;
    auto* pr = new (std::nothrow) Provider;
    if (!pr) return nullptr;
    pr->indexer = indexer;
    pr->provider = {&kProviderDesc, pr, provider_init, provider_destroy, provider_get_metadata, provider_get_extension};
    return &pr->provider;
}

const clap_preset_discovery_factory_t kDiscovery = {discovery_count, discovery_descriptor, discovery_create};

bool entry_init(const char*) { return true; }

void entry_deinit() {}

const void* entry_get_factory(const char* id) {
    if (std::strcmp(id, CLAP_PLUGIN_FACTORY_ID) == 0) return &kFactory;
    if (std::strcmp(id, CLAP_PRESET_DISCOVERY_FACTORY_ID) == 0) return &kDiscovery;
    return nullptr;
}

}  // namespace

extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry = {
    CLAP_VERSION_INIT,
    entry_init,
    entry_deinit,
    entry_get_factory,
};
