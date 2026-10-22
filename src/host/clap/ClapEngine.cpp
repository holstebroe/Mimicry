// CLAP backend (docs/spec/02_HOSTING.md section 2, WP1.2/1.3). The only place in the host that includes
// CLAP headers on the host side.

#include <clap/clap.h>
#include <clap/factory/preset-discovery.h>
#include <clap/ext/preset-load.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <deque>
#include <filesystem>
#include <format>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "host/Engine.hpp"
#include "host/FileHash.hpp"
#include "host/ParamUtil.hpp"
#include "host/clap/SharedLibrary.hpp"

namespace mimicry::host {

namespace {

union Event {
    clap_event_header_t header;
    clap_event_param_value_t param;
    clap_event_note_t note;
    clap_event_note_expression_t expression;
    clap_event_midi_t midi;
};

class ClapEngine final : public Engine {
public:
    ClapEngine(std::shared_ptr<ThreadRoles> roles) : roles_(std::move(roles)) {
        host_.clap_version = CLAP_VERSION;
        host_.host_data = this;
        host_.name = "Mimicry";
        host_.vendor = "Mimicry";
        host_.url = "";
        host_.version = MIMICRY_VERSION;
        host_.get_extension = &ClapEngine::host_get_extension;
        host_.request_restart = &ClapEngine::host_request_restart;
        host_.request_process = &ClapEngine::host_request_process;
        host_.request_callback = &ClapEngine::host_request_callback;
    }

    ~ClapEngine() override {
        close_editor();
        if (plugin_) plugin_->destroy(plugin_);
        plugin_ = nullptr;
        if (entry_) entry_->deinit();
    }

    expected<void, HostError> load(const std::filesystem::path& path, std::string_view uid) {
        auto lib = clap::SharedLibrary::open(path);
        if (!lib) return unexpected(HostError{lib.error()});
        lib_ = std::move(*lib);
        entry_ = static_cast<const clap_plugin_entry_t*>(lib_.symbol("clap_entry"));
        if (!entry_) return unexpected(HostError{std::format("'{}': no clap_entry symbol", path.string())});
        if (!entry_->init(path.string().c_str())) {
            entry_ = nullptr;
            return unexpected(HostError{std::format("'{}': clap_entry.init failed", path.string())});
        }
        const auto* factory = static_cast<const clap_plugin_factory_t*>(entry_->get_factory(CLAP_PLUGIN_FACTORY_ID));
        if (!factory) return unexpected(HostError{"no plugin factory"});

        const uint32_t count = factory->get_plugin_count(factory);
        const clap_plugin_descriptor_t* chosen = nullptr;
        std::string ids;
        for (uint32_t i = 0; i < count; ++i) {
            const auto* d = factory->get_plugin_descriptor(factory, i);
            if (!d || !d->id) continue;
            ids += (ids.empty() ? "" : ", ") + std::string(d->id);
            if (uid.empty() ? chosen == nullptr : uid == d->id) {
                if (!chosen) chosen = d;
            }
        }
        if (count == 0) return unexpected(HostError{"the file contains no plugins"});
        if (!uid.empty() && !chosen) return unexpected(HostError{std::format("no plugin '{}' in the file (has: {})", uid, ids)});
        if (uid.empty() && count > 1)
            return unexpected(HostError{std::format("the file contains several plugins, pick one by id: {}", ids)});

        plugin_ = factory->create_plugin(factory, &host_, chosen->id);
        if (!plugin_) return unexpected(HostError{std::format("create_plugin('{}') failed", chosen->id)});
        if (!plugin_->init(plugin_)) {
            plugin_->destroy(plugin_);
            plugin_ = nullptr;
            return unexpected(HostError{"plugin init failed"});
        }
        params_ = static_cast<const clap_plugin_params_t*>(plugin_->get_extension(plugin_, CLAP_EXT_PARAMS));
        state_ = static_cast<const clap_plugin_state_t*>(plugin_->get_extension(plugin_, CLAP_EXT_STATE));
        tail_ = static_cast<const clap_plugin_tail_t*>(plugin_->get_extension(plugin_, CLAP_EXT_TAIL));
        latency_ = static_cast<const clap_plugin_latency_t*>(plugin_->get_extension(plugin_, CLAP_EXT_LATENCY));
        audio_ports_ = static_cast<const clap_plugin_audio_ports_t*>(plugin_->get_extension(plugin_, CLAP_EXT_AUDIO_PORTS));
        note_ports_ = static_cast<const clap_plugin_note_ports_t*>(plugin_->get_extension(plugin_, CLAP_EXT_NOTE_PORTS));
        preset_load_ = static_cast<const clap_plugin_preset_load_t*>(plugin_->get_extension(plugin_, CLAP_EXT_PRESET_LOAD));
        if (!preset_load_)
            preset_load_ = static_cast<const clap_plugin_preset_load_t*>(plugin_->get_extension(plugin_, CLAP_EXT_PRESET_LOAD_COMPAT));
        // Offline rendering, as the VST3 backend's kOffline (spec 02 section 5): main thread, before activation.
        if (const auto* render = static_cast<const clap_plugin_render_t*>(plugin_->get_extension(plugin_, CLAP_EXT_RENDER));
            render && render->set && !(render->has_hard_realtime_requirement && render->has_hard_realtime_requirement(plugin_)))
            offline_ = render->set(plugin_, CLAP_RENDER_OFFLINE);

        info_.format = PluginFormat::Clap;
        info_.uid = chosen->id;
        info_.name = chosen->name ? chosen->name : chosen->id;
        info_.vendor = chosen->vendor ? chosen->vendor : "";
        info_.version_hash = plugin_version_hash(path);
        info_.kind = PluginKind::Unknown;
        for (const char* const* f = chosen->features; f && *f; ++f) {
            if (!std::strcmp(*f, CLAP_PLUGIN_FEATURE_INSTRUMENT)) info_.kind = PluginKind::Instrument;
            else if (!std::strcmp(*f, CLAP_PLUGIN_FEATURE_AUDIO_EFFECT) && info_.kind == PluginKind::Unknown)
                info_.kind = PluginKind::Effect;
        }

        // Audio and note port layout. Both are scanned while the plugin is deactivated.
        if (audio_ports_) {
            for (uint32_t i = 0; i < audio_ports_->count(plugin_, false); ++i) {
                clap_audio_port_info_t pi{};
                if (audio_ports_->get(plugin_, i, false, &pi)) out_channels_.push_back(static_cast<int>(pi.channel_count));
            }
            for (uint32_t i = 0; i < audio_ports_->count(plugin_, true); ++i) {
                clap_audio_port_info_t pi{};
                if (audio_ports_->get(plugin_, i, true, &pi)) in_channels_.push_back(static_cast<int>(pi.channel_count));
            }
        }
        if (out_channels_.empty()) return unexpected(HostError{"the plugin has no audio output port"});
        if (note_ports_ && note_ports_->count(plugin_, true) > 0) {
            clap_note_port_info_t ni{};
            if (note_ports_->get(plugin_, 0, true, &ni)) {
                clap_dialect_ = (ni.supported_dialects & CLAP_NOTE_DIALECT_CLAP) != 0;
                midi_dialect_ = (ni.supported_dialects & CLAP_NOTE_DIALECT_MIDI) != 0;
            }
        }
        if (info_.kind == PluginKind::Unknown)
            info_.kind = (!in_channels_.empty() && !(note_ports_ && note_ports_->count(plugin_, true) > 0))
                             ? PluginKind::Effect
                             : PluginKind::Instrument;
        enumerate();
        return {};
    }

    const PluginInfo& info() const override { return info_; }
    const std::vector<ParamDesc>& parameters() const override { return params_list_; }

    std::string value_text(std::uint32_t id, double coord) const override {
        const auto it = index_.find(id);
        if (it == index_.end() || !params_) return {};
        return text_at(params_list_[it->second], coord_to_plain(params_list_[it->second], coord));
    }

    expected<void, HostError> activate(int sample_rate, int max_block) override {
        if (!plugin_->activate(plugin_, sample_rate, 1, static_cast<uint32_t>(max_block)))
            return unexpected(HostError{"activate failed"});
        rate_ = sample_rate;
        max_block_ = max_block;
        active_ = true;
        steady_ = 0;
        const auto n = static_cast<std::size_t>(max_block);
        out_buffers_.assign(out_channels_.size(), {});
        out_ptrs_.assign(out_channels_.size(), {});
        out_bufs_.assign(out_channels_.size(), clap_audio_buffer_t{});
        for (std::size_t p = 0; p < out_channels_.size(); ++p) {
            const auto ch = static_cast<std::size_t>(std::max(out_channels_[p], 1));
            out_buffers_[p].assign(ch, std::vector<float>(n, 0.0f));
            for (auto& c : out_buffers_[p]) out_ptrs_[p].push_back(c.data());
            out_bufs_[p].data32 = out_ptrs_[p].data();
            out_bufs_[p].channel_count = static_cast<uint32_t>(ch);
        }
        in_buffers_.assign(in_channels_.size(), {});
        in_ptrs_.assign(in_channels_.size(), {});
        in_bufs_.assign(in_channels_.size(), clap_audio_buffer_t{});
        for (std::size_t p = 0; p < in_channels_.size(); ++p) {
            const auto ch = static_cast<std::size_t>(std::max(in_channels_[p], 1));
            in_buffers_[p].assign(ch, std::vector<float>(n, 0.0f));
            for (auto& c : in_buffers_[p]) in_ptrs_[p].push_back(c.data());
            in_bufs_[p].data32 = in_ptrs_[p].data();
            in_bufs_[p].channel_count = static_cast<uint32_t>(ch);
            in_bufs_[p].constant_mask = ~0ull;
        }
        return {};
    }

    void deactivate() override {
        if (active_ && plugin_) plugin_->deactivate(plugin_);
        active_ = false;
    }

    expected<std::vector<std::uint8_t>, HostError> save_state() override {
        if (!state_) return unexpected(HostError{"the plugin has no state extension"});
        std::vector<std::uint8_t> blob;
        clap_ostream_t os{&blob, [](const clap_ostream_t* s, const void* buf, uint64_t size) -> int64_t {
                              auto* v = static_cast<std::vector<std::uint8_t>*>(s->ctx);
                              const auto* p = static_cast<const std::uint8_t*>(buf);
                              v->insert(v->end(), p, p + size);
                              return static_cast<int64_t>(size);
                          }};
        if (!state_->save(plugin_, &os)) return unexpected(HostError{"state save failed"});
        return blob;
    }

    expected<void, HostError> load_state(const std::vector<std::uint8_t>& blob) override {
        if (!state_) return unexpected(HostError{"the plugin has no state extension"});
        struct Src {
            const std::vector<std::uint8_t>* data;
            std::size_t pos;
        } src{&blob, 0};
        clap_istream_t is{&src, [](const clap_istream_t* s, void* buf, uint64_t size) -> int64_t {
                              auto* sr = static_cast<Src*>(s->ctx);
                              const std::size_t n = std::min<std::size_t>(size, sr->data->size() - sr->pos);
                              if (n) std::memcpy(buf, sr->data->data() + sr->pos, n);
                              sr->pos += n;
                              return static_cast<int64_t>(n);
                          }};
        if (!state_->load(plugin_, &is)) return unexpected(HostError{"state load failed"});
        return {};
    }

    bool rescan_parameters() override {
        if (!rescan_pending_.exchange(false)) return false;
        const auto before = params_list_;
        enumerate();
        if (before.size() != params_list_.size()) return true;
        for (std::size_t i = 0; i < before.size(); ++i)
            if (before[i].id != params_list_[i].id || before[i].name != params_list_[i].name ||
                before[i].min != params_list_[i].min || before[i].max != params_list_[i].max)
                return true;
        return false;
    }

    bool take_restart_request() override { return restart_.exchange(false); }

    // Editor (Windows only): the `gui` extension with an embedded win32 window; the host side answers the plugin's
    // resize requests (they may come from another thread: applied on the main thread in pump_main_thread).
    expected<EditorSize, HostError> open_editor(void* parent, EditorEvents events) override {
#if defined(_WIN32)
        gui_ = static_cast<const clap_plugin_gui_t*>(plugin_->get_extension(plugin_, CLAP_EXT_GUI));
        if (!gui_) return unexpected(HostError{"the plugin has no editor"});
        close_editor();
        if (!gui_->is_api_supported(plugin_, CLAP_WINDOW_API_WIN32, false))
            return unexpected(HostError{"the plugin's editor does not support a Win32 window"});
        if (!gui_->create(plugin_, CLAP_WINDOW_API_WIN32, false)) return unexpected(HostError{"the plugin's editor could not be created"});
        gui_created_ = true;
        editor_events_ = std::move(events);
        gui_resize_pending_ = false;
        clap_window_t window{};
        window.api = CLAP_WINDOW_API_WIN32;
        window.win32 = parent;
        if (!gui_->set_parent(plugin_, &window)) {
            close_editor();
            return unexpected(HostError{"the plugin's editor could not attach to the window"});
        }
        uint32_t w = 800, h = 600;
        if (!gui_->get_size(plugin_, &w, &h)) {
            w = 800;
            h = 600;
        }
        gui_->show(plugin_);
        EditorSize size;
        size.width = static_cast<int>(w);
        size.height = static_cast<int>(h);
        size.resizable = gui_->can_resize(plugin_);
        return size;
#else
        (void)parent;
        (void)events;
        return unexpected(HostError{"plugin editors are hosted on Windows only"});
#endif
    }
    void editor_resized(int width, int height) override {
        if (!gui_ || !gui_created_) return;
        uint32_t w = static_cast<uint32_t>(std::max(width, 1)), h = static_cast<uint32_t>(std::max(height, 1));
        gui_->adjust_size(plugin_, &w, &h);
        gui_->set_size(plugin_, w, h);
    }
    void close_editor() override {
        if (!gui_ || !gui_created_) return;
        gui_->hide(plugin_);
        gui_->destroy(plugin_);
        gui_created_ = false;
        editor_events_ = {};
    }
    int take_change_signals() override { return change_signals_.exchange(0); }

    // --- presets (docs/spec/02_HOSTING.md section 7) ---

    std::vector<PresetRef> presets() override {
        std::vector<PresetRef> out = discover_presets();
        if (midi_dialect_)  // blind: the plugin cannot say which programs exist; the harvest verifies each
            for (int i = 0; i < 128; ++i) {
                PresetRef r;
                r.method = PresetMethod::MidiProgram;
                r.index = i;
                r.count = 128;
                out.push_back(std::move(r));
            }
        return out;
    }

    expected<void, HostError> load_preset(const PresetRef& ref) override {
        {  // every load starts clean: an earlier load's error, pending flag or late callback never carries over
            std::lock_guard lock(preset_mutex_);
            preset_pending_ = false;
            preset_error_.clear();
            preset_location_.clear();
            preset_key_.clear();
        }
        if (ref.method != PresetMethod::ClapPreset) return Engine::load_preset(ref);
        if (!preset_load_ || !preset_load_->from_location) return unexpected(HostError{"the plugin has no preset-load extension"});
        {
            std::lock_guard lock(preset_mutex_);
            preset_pending_ = true;
            preset_location_ = ref.location_is_file ? ref.location : std::string();
            preset_key_ = ref.load_key;
        }
        const uint32_t kind = ref.location_is_file ? CLAP_PRESET_DISCOVERY_LOCATION_FILE : CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN;
        if (!preset_load_->from_location(plugin_, kind, ref.location_is_file ? ref.location.c_str() : nullptr,
                                         ref.load_key.empty() ? nullptr : ref.load_key.c_str())) {
            std::lock_guard lock(preset_mutex_);
            preset_pending_ = false;
            return unexpected(HostError{preset_error_.empty() ? std::string("preset-load.from_location failed") : preset_error_});
        }
        return {};
    }

    PresetLoadStatus preset_status() override {
        std::lock_guard lock(preset_mutex_);
        return {preset_pending_, preset_error_};
    }

    std::optional<double> param_coord(std::uint32_t id) const override {
        const auto it = index_.find(id);
        double plain = 0;
        if (it == index_.end() || !params_ || !params_->get_value(plugin_, id, &plain)) return std::nullopt;
        return plain_to_coord(params_list_[it->second], plain);
    }

    bool midi_program_change() const override { return midi_dialect_; }

    void pump_main_thread() override {
        if (callback_.exchange(false) && plugin_) plugin_->on_main_thread(plugin_);
        if (gui_created_ && gui_resize_pending_.exchange(false) && editor_events_.resize) {
            // The plugin asked for a new client size: the window follows, then the plugin is told it has it.
            const int w = static_cast<int>(gui_resize_w_.load()), h = static_cast<int>(gui_resize_h_.load());
            editor_events_.resize(w, h);
            gui_->set_size(plugin_, static_cast<uint32_t>(w), static_cast<uint32_t>(h));
        }
    }

    expected<void, HostError> start() override {
        if (!plugin_->start_processing(plugin_)) return unexpected(HostError{"start_processing failed"});
        return {};
    }

    void stop() override {
        if (plugin_) plugin_->stop_processing(plugin_);
    }

    expected<void, HostError> process(const Block& block, float* left, float* right) override {
        const std::size_t frames = static_cast<std::size_t>(block.frames);
        if (block.frames > max_block_) return unexpected(HostError{"block larger than the activated maximum"});

        events_.clear();
        for (const ParamValue& pv : block.params) {
            const auto it = index_.find(pv.id);
            if (it == index_.end()) continue;
            Event e{};
            e.param.header = {sizeof(clap_event_param_value_t), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0};
            e.param.param_id = pv.id;
            e.param.cookie = nullptr;
            e.param.note_id = -1;
            e.param.port_index = -1;
            e.param.channel = -1;
            e.param.key = -1;
            e.param.value = coord_to_plain(params_list_[it->second], pv.coord);
            events_.push_back(e);
        }
        if (block.program >= 0 && midi_dialect_) {  // bank select (CC 0 / CC 32), then the program change
            auto midi = [&](uint8_t a, uint8_t b, uint8_t c) {
                Event m{};
                m.midi.header = {sizeof(clap_event_midi_t), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI, 0};
                m.midi.port_index = 0;
                m.midi.data[0] = a;
                m.midi.data[1] = b;
                m.midi.data[2] = c;
                events_.push_back(m);
            };
            if (block.bank >= 0) {
                midi(0xB0, 0, static_cast<uint8_t>((block.bank >> 7) & 0x7F));
                midi(0xB0, 32, static_cast<uint8_t>(block.bank & 0x7F));
            }
            midi(0xC0, static_cast<uint8_t>(block.program & 0x7F), 0);
        }
        for (const NoteEvent& n : block.notes) {
            if (n.set_bend && midi_dialect_) {  // MIDI pitch bend, channel 0, ahead of the note
                const int v = std::clamp(static_cast<int>(std::lround(8192.0 + n.bend * 8192.0)), 0, 16383);
                Event b{};
                b.midi.header = {sizeof(clap_event_midi_t), n.frame, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI, 0};
                b.midi.port_index = 0;
                b.midi.data[0] = 0xE0;
                b.midi.data[1] = static_cast<uint8_t>(v & 0x7F);
                b.midi.data[2] = static_cast<uint8_t>(v >> 7);
                events_.push_back(b);
            }
            Event e{};
            int key = n.key;
            double cents = n.cents;
            if (!clap_dialect_ && std::fabs(cents) > 0) {  // no expressions: nearest key, for the off as well
                key += static_cast<int>(std::lround(cents / 100.0));
                cents = 0;
            }
            e.note.header = {sizeof(clap_event_note_t), n.frame, CLAP_CORE_EVENT_SPACE_ID,
                             static_cast<uint16_t>(n.on ? CLAP_EVENT_NOTE_ON : CLAP_EVENT_NOTE_OFF), 0};
            e.note.note_id = n.note_id;
            e.note.port_index = 0;
            e.note.channel = 0;
            e.note.key = static_cast<int16_t>(key);
            e.note.velocity = n.velocity;
            events_.push_back(e);
            if (n.on && std::fabs(cents) > 0) {
                Event x{};
                x.expression.header = {sizeof(clap_event_note_expression_t), n.frame, CLAP_CORE_EVENT_SPACE_ID,
                                       CLAP_EVENT_NOTE_EXPRESSION, 0};
                x.expression.expression_id = CLAP_NOTE_EXPRESSION_TUNING;
                x.expression.note_id = n.note_id;
                x.expression.port_index = 0;
                x.expression.channel = 0;
                x.expression.key = static_cast<int16_t>(key);
                x.expression.value = cents / 100.0;
                events_.push_back(x);
            }
        }

        clap_input_events_t in{&events_, [](const clap_input_events_t* l) -> uint32_t {
                                   return static_cast<uint32_t>(static_cast<std::vector<Event>*>(l->ctx)->size());
                               },
                               [](const clap_input_events_t* l, uint32_t i) -> const clap_event_header_t* {
                                   return &(*static_cast<std::vector<Event>*>(l->ctx))[i].header;
                               }};
        clap_output_events_t out{nullptr, [](const clap_output_events_t*, const clap_event_header_t*) { return true; }};

        clap_event_transport_t transport{};
        transport.header = {sizeof transport, 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_TRANSPORT, 0};
        transport.flags = CLAP_TRANSPORT_HAS_TEMPO | CLAP_TRANSPORT_HAS_BEATS_TIMELINE |
                          CLAP_TRANSPORT_HAS_SECONDS_TIMELINE | CLAP_TRANSPORT_HAS_TIME_SIGNATURE |
                          CLAP_TRANSPORT_IS_PLAYING;
        transport.tempo = 120.0;
        const double seconds = static_cast<double>(steady_) / rate_;
        transport.song_pos_seconds = static_cast<clap_sectime>(seconds * CLAP_SECTIME_FACTOR);
        transport.song_pos_beats = static_cast<clap_beattime>(seconds * 2.0 * CLAP_BEATTIME_FACTOR);
        transport.tsig_num = 4;
        transport.tsig_denom = 4;

        clap_process_t pr{};
        pr.steady_time = steady_;
        pr.frames_count = static_cast<uint32_t>(frames);
        pr.transport = &transport;
        pr.audio_inputs = in_bufs_.empty() ? nullptr : in_bufs_.data();
        pr.audio_inputs_count = static_cast<uint32_t>(in_bufs_.size());
        pr.audio_outputs = out_bufs_.data();
        pr.audio_outputs_count = static_cast<uint32_t>(out_bufs_.size());
        pr.in_events = &in;
        pr.out_events = &out;
        for (auto& port : out_buffers_)
            for (auto& ch : port) std::fill_n(ch.begin(), frames, 0.0f);

        const clap_process_status status = plugin_->process(plugin_, &pr);
        steady_ += block.frames;
        if (status == CLAP_PROCESS_ERROR) return unexpected(HostError{"process returned CLAP_PROCESS_ERROR"});

        // Main output port to stereo; a mono port feeds both channels.
        const auto& main = out_buffers_[0];
        const auto& l = main[0];
        const auto& r = main.size() > 1 ? main[1] : main[0];
        std::copy_n(l.begin(), frames, left);
        std::copy_n(r.begin(), frames, right);
        return {};
    }

    int tail_samples() const override {
        if (!tail_) return 0;
        const uint32_t t = tail_->get(plugin_);
        return t >= static_cast<uint32_t>(INT32_MAX) ? INT32_MAX : static_cast<int>(t);
    }
    int latency_samples() const override { return (latency_ && active_) ? static_cast<int>(latency_->get(plugin_)) : 0; }

private:
    std::string text_at(const ParamDesc& d, double plain) const {
        char buf[256] = {};
        if (!params_->value_to_text(plugin_, d.id, plain, buf, sizeof buf)) return {};
        return buf;
    }

    void enumerate() {
        params_list_.clear();
        index_.clear();
        if (!params_) return;
        const uint32_t n = params_->count(plugin_);
        for (uint32_t i = 0; i < n; ++i) {
            clap_param_info_t pi{};
            if (!params_->get_info(plugin_, i, &pi)) continue;
            ParamDesc d;
            d.id = pi.id;
            d.name = pi.name;
            d.module = pi.module;
            d.min = pi.min_value;
            d.max = pi.max_value;
            d.default_plain = pi.default_value;
            const bool stepped = (pi.flags & CLAP_PARAM_IS_STEPPED) != 0;
            d.steps = stepped ? std::max(2, static_cast<int>(std::lround(pi.max_value - pi.min_value)) + 1) : 0;
            d.flags.hidden = (pi.flags & CLAP_PARAM_IS_HIDDEN) != 0;
            d.flags.readonly = (pi.flags & CLAP_PARAM_IS_READONLY) != 0;
            d.flags.bypass = (pi.flags & CLAP_PARAM_IS_BYPASS) != 0;
            d.flags.automatable = (pi.flags & CLAP_PARAM_IS_AUTOMATABLE) != 0;
            // CLAP has no program-change flag; a stepped parameter named like one is pinned [I].
            d.flags.program_change = stepped && looks_like_program_param(d.name);
            // Texts first (units decide log/linear), then the linear coordinate of the default.
            d.log_scale = false;
            for (int k = 0; k < 5; ++k) {
                const double plain = d.min + (d.max - d.min) * (k * 0.25);
                d.texts[static_cast<std::size_t>(k)] = text_at(d, plain);
            }
            d.units = units_from_texts(d.texts);
            d.log_scale = use_log_scale(d.min, d.max, d.steps, d.units);
            if (d.log_scale)  // texts at the 25/50/75 % points of the coordinate, not of the plain range
                for (int k = 1; k < 4; ++k)
                    d.texts[static_cast<std::size_t>(k)] = text_at(d, coord_to_plain(d, k * 0.25));
            d.default_coord = plain_to_coord(d, d.default_plain);
            index_[d.id] = params_list_.size();
            params_list_.push_back(std::move(d));
        }
    }

    // CLAP preset discovery (factory/preset-discovery.h): every provider of the entry, its declared plugin
    // locations and the files of its file locations with the declared extensions, kept when the preset names
    // this plugin (or names none).
    std::vector<PresetRef> discover_presets() {
        std::vector<PresetRef> out;
        if (!entry_) return out;
        const auto* factory = static_cast<const clap_preset_discovery_factory_t*>(entry_->get_factory(CLAP_PRESET_DISCOVERY_FACTORY_ID));
        if (!factory)
            factory = static_cast<const clap_preset_discovery_factory_t*>(entry_->get_factory(CLAP_PRESET_DISCOVERY_FACTORY_ID_COMPAT));
        if (!factory) return out;

        struct Location {
            uint32_t kind;
            std::string path;
        };
        struct Indexer {
            std::vector<std::string> extensions;
            std::vector<Location> locations;
        };
        struct Receiver {
            std::vector<PresetRef>* out;
            const std::string* plugin_id;
            PresetRef base;
            std::vector<int> mentions;  // per preset: -1 none named, 0 others only, 1 this plugin
        };
        for (uint32_t pi = 0; pi < factory->count(factory); ++pi) {
            const auto* desc = factory->get_descriptor(factory, pi);
            if (!desc || !desc->id) continue;
            Indexer ix;
            clap_preset_discovery_indexer_t indexer{};
            indexer.clap_version = CLAP_VERSION;
            indexer.name = "Mimicry";
            indexer.vendor = "Mimicry";
            indexer.url = "";
            indexer.version = MIMICRY_VERSION;
            indexer.indexer_data = &ix;
            indexer.declare_filetype = [](const clap_preset_discovery_indexer_t* i, const clap_preset_discovery_filetype_t* ft) {
                if (ft && ft->file_extension) static_cast<Indexer*>(i->indexer_data)->extensions.emplace_back(ft->file_extension);
                return true;
            };
            indexer.declare_location = [](const clap_preset_discovery_indexer_t* i, const clap_preset_discovery_location_t* loc) {
                if (loc) static_cast<Indexer*>(i->indexer_data)->locations.push_back({loc->kind, loc->location ? loc->location : ""});
                return true;
            };
            indexer.declare_soundpack = [](const clap_preset_discovery_indexer_t*, const clap_preset_discovery_soundpack_t*) { return true; };
            indexer.get_extension = [](const clap_preset_discovery_indexer_t*, const char*) -> const void* { return nullptr; };
            const auto* provider = factory->create(factory, &indexer, desc->id);
            if (!provider) continue;
            if (!provider->init(provider)) {
                provider->destroy(provider);
                continue;
            }

            Receiver rc{&out, &info_.uid, {}, {}};
            clap_preset_discovery_metadata_receiver_t receiver{};
            receiver.receiver_data = &rc;
            receiver.on_error = [](const clap_preset_discovery_metadata_receiver_t*, int32_t, const char*) {};
            receiver.begin_preset = [](const clap_preset_discovery_metadata_receiver_t* r, const char* name, const char* key) {
                auto* c = static_cast<Receiver*>(r->receiver_data);
                PresetRef ref = c->base;
                ref.name = name ? name : "";
                ref.load_key = key ? key : "";
                c->out->push_back(std::move(ref));
                c->mentions.push_back(-1);
                return true;
            };
            receiver.add_plugin_id = [](const clap_preset_discovery_metadata_receiver_t* r, const clap_universal_plugin_id_t* id) {
                auto* c = static_cast<Receiver*>(r->receiver_data);
                if (c->mentions.empty() || !id || !id->abi || !id->id) return;
                const bool ours = !std::strcmp(id->abi, "clap") && *c->plugin_id == id->id;
                c->mentions.back() = std::max(c->mentions.back(), ours ? 1 : 0);
            };
            receiver.set_soundpack_id = [](const clap_preset_discovery_metadata_receiver_t*, const char*) {};
            receiver.set_flags = [](const clap_preset_discovery_metadata_receiver_t*, uint32_t) {};
            receiver.add_creator = [](const clap_preset_discovery_metadata_receiver_t*, const char*) {};
            receiver.set_description = [](const clap_preset_discovery_metadata_receiver_t*, const char*) {};
            receiver.set_timestamps = [](const clap_preset_discovery_metadata_receiver_t*, clap_timestamp, clap_timestamp) {};
            receiver.add_feature = [](const clap_preset_discovery_metadata_receiver_t*, const char*) {};
            receiver.add_extra_info = [](const clap_preset_discovery_metadata_receiver_t*, const char*, const char*) {};

            const std::size_t first = out.size();
            for (const Location& loc : ix.locations) {
                if (loc.kind == CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN) {
                    rc.base = PresetRef{};
                    rc.base.method = PresetMethod::ClapPreset;
                    provider->get_metadata(provider, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr, &receiver);
                    continue;
                }
                // A file location is a file or a folder to crawl (only the declared extensions, if any).
                std::error_code ec;
                std::vector<std::filesystem::path> files;
                if (std::filesystem::is_regular_file(loc.path, ec)) {
                    files.emplace_back(loc.path);
                } else if (std::filesystem::is_directory(loc.path, ec)) {
                    for (auto it = std::filesystem::recursive_directory_iterator(loc.path, ec);
                         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
                        if (!it->is_regular_file(ec)) continue;
                        const std::string ext = it->path().extension().string();
                        const bool wanted = ix.extensions.empty() || std::ranges::any_of(ix.extensions, [&](const std::string& e) {
                            return ext == "." + e || ext == e;
                        });
                        if (wanted) files.push_back(it->path());
                    }
                    std::ranges::sort(files);
                }
                for (const auto& f : files) {
                    rc.base = PresetRef{};
                    rc.base.method = PresetMethod::ClapPreset;
                    rc.base.location = f.string();
                    rc.base.location_is_file = true;
                    const std::size_t before = out.size();
                    provider->get_metadata(provider, CLAP_PRESET_DISCOVERY_LOCATION_FILE, rc.base.location.c_str(), &receiver);
                    for (std::size_t k = before; k < out.size(); ++k)
                        if (out[k].name.empty()) out[k].name = f.stem().string();
                }
            }
            provider->destroy(provider);
            // Presets that name only other plugins are not this plugin's.
            std::vector<PresetRef> kept(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(first));
            for (std::size_t k = first; k < out.size(); ++k)
                if (rc.mentions[k - first] != 0) kept.push_back(std::move(out[k]));
            out = std::move(kept);
        }
        return out;
    }

    // --- host callbacks ---
    static ClapEngine* self(const clap_host_t* h) { return static_cast<ClapEngine*>(h->host_data); }
    static void host_request_restart(const clap_host_t* h) { self(h)->restart_ = true; }
    static void host_request_process(const clap_host_t*) {}
    static void host_request_callback(const clap_host_t* h) { self(h)->callback_ = true; }
    static void gui_resize_hints_changed(const clap_host_t*) {}
    static bool gui_request_resize(const clap_host_t* h, uint32_t w, uint32_t hh) {
        auto* e = self(h);
        e->gui_resize_w_ = w;
        e->gui_resize_h_ = hh;
        e->gui_resize_pending_ = true;  // applied on the main thread (pump_main_thread)
        return true;
    }
    static bool gui_request_show(const clap_host_t*) { return true; }
    static bool gui_request_hide(const clap_host_t*) { return true; }
    static void gui_closed(const clap_host_t* h, bool) {
        auto* e = self(h);
        if (e->editor_events_.closed) e->editor_events_.closed();
    }

    static bool is_main_thread(const clap_host_t* h) { return std::this_thread::get_id() == self(h)->roles_->main; }
    static bool is_audio_thread(const clap_host_t* h) { return std::this_thread::get_id() == self(h)->roles_->audio.load(); }
    static void params_rescan(const clap_host_t* h, clap_param_rescan_flags flags) {
        if (flags & (CLAP_PARAM_RESCAN_ALL | CLAP_PARAM_RESCAN_INFO | CLAP_PARAM_RESCAN_TEXT)) {
            self(h)->rescan_pending_ = true;
            self(h)->restart_ = true;
        }
    }
    static void params_clear(const clap_host_t*, clap_id, clap_param_clear_flags) {}
    static void params_request_flush(const clap_host_t*) {}
    static void state_mark_dirty(const clap_host_t*) {}
    // A callback counts only for the load in flight: one arriving late for an earlier preset is ignored.
    bool current_preset(const char* location, const char* key) const {
        if (!preset_pending_ || preset_key_ != (key ? key : "")) return false;
        return preset_location_.empty() || preset_location_ == (location ? location : "");
    }
    static void preset_on_error(const clap_host_t* h, uint32_t, const char* location, const char* key, int32_t, const char* msg) {
        auto* e = self(h);
        std::lock_guard lock(e->preset_mutex_);
        if (!e->current_preset(location, key)) return;
        e->preset_pending_ = false;
        e->preset_error_ = msg && *msg ? msg : "unknown error";
    }
    static void preset_loaded(const clap_host_t* h, uint32_t, const char* location, const char* key) {
        auto* e = self(h);
        ++e->change_signals_;  // also a load the plugin's own browser made (watch mode)
        std::lock_guard lock(e->preset_mutex_);
        if (!e->current_preset(location, key)) return;
        e->preset_pending_ = false;
    }
    static void latency_changed(const clap_host_t*) {}
    static void tail_changed(const clap_host_t*) {}
    static void log_msg(const clap_host_t* h, clap_log_severity, const char* msg) {
        auto* e = self(h);
        std::lock_guard lock(e->log_mutex_);
        e->log_.emplace_back(msg ? msg : "");
        if (e->log_.size() > 20) e->log_.pop_front();
    }

    static const void* host_get_extension(const clap_host_t*, const char* id) {
        static const clap_host_thread_check_t thread_check{is_main_thread, is_audio_thread};
        static const clap_host_params_t params{params_rescan, params_clear, params_request_flush};
        static const clap_host_state_t state{state_mark_dirty};
        static const clap_host_latency_t latency{latency_changed};
        static const clap_host_tail_t tail{tail_changed};
        static const clap_host_log_t log{log_msg};
        static const clap_host_preset_load_t preset_load{preset_on_error, preset_loaded};
        static const clap_host_gui_t gui{gui_resize_hints_changed, gui_request_resize, gui_request_show, gui_request_hide, gui_closed};
        if (!std::strcmp(id, CLAP_EXT_GUI)) return &gui;
        if (!std::strcmp(id, CLAP_EXT_THREAD_CHECK)) return &thread_check;
        if (!std::strcmp(id, CLAP_EXT_PARAMS)) return &params;
        if (!std::strcmp(id, CLAP_EXT_STATE)) return &state;
        if (!std::strcmp(id, CLAP_EXT_LATENCY)) return &latency;
        if (!std::strcmp(id, CLAP_EXT_TAIL)) return &tail;
        if (!std::strcmp(id, CLAP_EXT_LOG)) return &log;
        if (!std::strcmp(id, CLAP_EXT_PRESET_LOAD) || !std::strcmp(id, CLAP_EXT_PRESET_LOAD_COMPAT)) return &preset_load;
        return nullptr;
    }

    std::shared_ptr<ThreadRoles> roles_;
    clap::SharedLibrary lib_;
    const clap_plugin_entry_t* entry_ = nullptr;
    clap_host_t host_{};
    const clap_plugin_t* plugin_ = nullptr;
    const clap_plugin_params_t* params_ = nullptr;
    const clap_plugin_state_t* state_ = nullptr;
    const clap_plugin_tail_t* tail_ = nullptr;
    const clap_plugin_latency_t* latency_ = nullptr;
    const clap_plugin_audio_ports_t* audio_ports_ = nullptr;
    const clap_plugin_note_ports_t* note_ports_ = nullptr;
    const clap_plugin_preset_load_t* preset_load_ = nullptr;
    const clap_plugin_gui_t* gui_ = nullptr;
    bool gui_created_ = false;
    EditorEvents editor_events_;
    std::atomic<bool> gui_resize_pending_{false};
    std::atomic<std::uint32_t> gui_resize_w_{0}, gui_resize_h_{0};
    std::atomic<int> change_signals_{0};

    PluginInfo info_;
    std::vector<ParamDesc> params_list_;
    std::unordered_map<std::uint32_t, std::size_t> index_;
    std::vector<int> in_channels_, out_channels_;
    bool clap_dialect_ = true;
    bool offline_ = false;  // the plugin accepted CLAP_RENDER_OFFLINE
    bool midi_dialect_ = false;

    int rate_ = 48000;
    int max_block_ = 512;
    bool active_ = false;
    std::int64_t steady_ = 0;
    std::vector<std::vector<std::vector<float>>> out_buffers_, in_buffers_;
    std::vector<std::vector<float*>> out_ptrs_, in_ptrs_;
    std::vector<clap_audio_buffer_t> out_bufs_, in_bufs_;
    std::vector<Event> events_;

    std::atomic<bool> restart_{false}, callback_{false}, rescan_pending_{false};
    std::mutex preset_mutex_;
    bool preset_pending_ = false;
    std::string preset_error_;
    std::string preset_location_, preset_key_;  // the load in flight
    std::mutex log_mutex_;
    std::deque<std::string> log_;
};

}  // namespace

expected<std::unique_ptr<Engine>, HostError> open_clap_engine(const std::filesystem::path& path, std::string_view uid,
                                                              std::shared_ptr<ThreadRoles> roles) {
    auto engine = std::make_unique<ClapEngine>(std::move(roles));
    if (auto r = engine->load(path, uid); !r) return unexpected(r.error());
    return std::unique_ptr<Engine>(std::move(engine));
}

}  // namespace mimicry::host
