// CLAP backend (docs/spec/02_HOSTING.md section 2, WP1.2/1.3). The only place in the host that includes
// CLAP headers on the host side.

#include <clap/clap.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <deque>
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
};

std::string lower(std::string s) {
    std::ranges::transform(s, s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool looks_like_program_param(const std::string& name) {
    const std::string n = lower(name);
    return n == "program" || n == "preset" || n == "patch" || n == "program change";
}

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
            if (note_ports_->get(plugin_, 0, true, &ni)) clap_dialect_ = (ni.supported_dialects & CLAP_NOTE_DIALECT_CLAP) != 0;
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

    void pump_main_thread() override {
        if (callback_.exchange(false) && plugin_) plugin_->on_main_thread(plugin_);
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
        for (const NoteEvent& n : block.notes) {
            Event e{};
            int key = n.key;
            double cents = n.cents;
            if (n.on && !clap_dialect_ && std::fabs(cents) > 0) {  // no expressions: nearest key
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

    // --- host callbacks ---
    static ClapEngine* self(const clap_host_t* h) { return static_cast<ClapEngine*>(h->host_data); }
    static void host_request_restart(const clap_host_t* h) { self(h)->restart_ = true; }
    static void host_request_process(const clap_host_t*) {}
    static void host_request_callback(const clap_host_t* h) { self(h)->callback_ = true; }

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
        if (!std::strcmp(id, CLAP_EXT_THREAD_CHECK)) return &thread_check;
        if (!std::strcmp(id, CLAP_EXT_PARAMS)) return &params;
        if (!std::strcmp(id, CLAP_EXT_STATE)) return &state;
        if (!std::strcmp(id, CLAP_EXT_LATENCY)) return &latency;
        if (!std::strcmp(id, CLAP_EXT_TAIL)) return &tail;
        if (!std::strcmp(id, CLAP_EXT_LOG)) return &log;
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

    PluginInfo info_;
    std::vector<ParamDesc> params_list_;
    std::unordered_map<std::uint32_t, std::size_t> index_;
    std::vector<int> in_channels_, out_channels_;
    bool clap_dialect_ = true;

    int rate_ = 48000;
    int max_block_ = 512;
    bool active_ = false;
    std::int64_t steady_ = 0;
    std::vector<std::vector<std::vector<float>>> out_buffers_, in_buffers_;
    std::vector<std::vector<float*>> out_ptrs_, in_ptrs_;
    std::vector<clap_audio_buffer_t> out_bufs_, in_bufs_;
    std::vector<Event> events_;

    std::atomic<bool> restart_{false}, callback_{false}, rescan_pending_{false};
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
