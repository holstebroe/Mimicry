// mimicry_test_synth as a CLAP plugin (docs/spec/07_OUTPUT_VALIDATION.md section 5). The DSP, the parameter
// table and the planted hazards are in TestSynthCore.hpp, shared with the VST3 build.

#include <clap/clap.h>

#include <cstdio>
#include <cstring>
#include <new>
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
    Synth synth;
    bool processing = false;
};

Plugin* self(const clap_plugin_t* p) { return static_cast<Plugin*>(p->plugin_data); }

// --- params ----------------------------------------------------------------------------------

uint32_t params_count(const clap_plugin_t*) { return kParamCount; }

bool params_get_info(const clap_plugin_t*, uint32_t index, clap_param_info_t* out) {
    if (index >= kParamCount) return false;
    const ParamInfo& p = kParams[index];
    std::memset(out, 0, sizeof *out);
    out->id = p.id;
    out->flags = CLAP_PARAM_IS_AUTOMATABLE;
    if (p.stepped) out->flags |= CLAP_PARAM_IS_STEPPED;
    if (p.hidden) out->flags |= CLAP_PARAM_IS_HIDDEN;
    if (p.readonly) out->flags = CLAP_PARAM_IS_READONLY;
    if (p.bypass) out->flags |= CLAP_PARAM_IS_BYPASS;
    std::snprintf(out->name, sizeof out->name, "%s", p.name);
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
    info->supported_dialects = CLAP_NOTE_DIALECT_CLAP;
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
        const double v = pl.synth.value(static_cast<ParamId>(i));
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

bool state_load(const clap_plugin_t* plugin, const clap_istream_t* stream) {
    Plugin& pl = *self(plugin);
    std::vector<char> buf(sizeof kStateMagic + kParamCount * sizeof(double));
    int64_t done = 0;
    while (done < static_cast<int64_t>(buf.size())) {
        const int64_t n = stream->read(stream, buf.data() + done, buf.size() - static_cast<uint64_t>(done));
        if (n <= 0) return false;
        done += n;
    }
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

const clap_plugin_state_t kState = {state_save, state_load};

uint32_t tail_get(const clap_plugin_t* plugin) {
    return static_cast<uint32_t>(self(plugin)->synth.release_tail_seconds() * 48000.0);
}
const clap_plugin_tail_t kTail = {tail_get};

uint32_t latency_get(const clap_plugin_t*) { return 0; }
const clap_plugin_latency_t kLatency = {latency_get};

// --- plugin ----------------------------------------------------------------------------------

bool plugin_init(const clap_plugin_t* plugin) {
    Plugin& pl = *self(plugin);
    pl.synth.on_restart_request = [&pl] {
        if (pl.host && pl.host->request_restart) pl.host->request_restart(pl.host);
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

const void* plugin_get_extension(const clap_plugin_t*, const char* id) {
    if (!std::strcmp(id, CLAP_EXT_PARAMS)) return &kParamsExt;
    if (!std::strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &kAudioPorts;
    if (!std::strcmp(id, CLAP_EXT_NOTE_PORTS)) return &kNotePorts;
    if (!std::strcmp(id, CLAP_EXT_STATE)) return &kState;
    if (!std::strcmp(id, CLAP_EXT_TAIL)) return &kTail;
    if (!std::strcmp(id, CLAP_EXT_LATENCY)) return &kLatency;
    return nullptr;
}

void plugin_on_main_thread(const clap_plugin_t*) {}

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

bool entry_init(const char*) { return true; }

void entry_deinit() {}

const void* entry_get_factory(const char* id) {
    return std::strcmp(id, CLAP_PLUGIN_FACTORY_ID) == 0 ? &kFactory : nullptr;
}

}  // namespace

extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry = {
    CLAP_VERSION_INIT,
    entry_init,
    entry_deinit,
    entry_get_factory,
};
