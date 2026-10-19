// VST3 backend (docs/spec/02_HOSTING.md section 2, WP1.7/1.8). The only host code that includes VST3
// headers. Built on the SDK's hosting module: Module, PlugProvider, HostApplication, ParameterChanges,
// EventList, HostProcessData.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <format>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "base/source/fstring.h"
#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstmidicontrollers.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "pluginterfaces/vst/ivstunits.h"
#include "public.sdk/source/common/memorystream.h"
#include "public.sdk/source/vst/hosting/eventlist.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/hosting/module.h"
#include "public.sdk/source/vst/hosting/parameterchanges.h"
#include "public.sdk/source/vst/hosting/plugprovider.h"
#include "public.sdk/source/vst/hosting/processdata.h"

#include "host/Engine.hpp"
#include "host/FileHash.hpp"
#include "host/ParamUtil.hpp"

namespace mimicry::host {

namespace {

using namespace Steinberg;
using namespace Steinberg::Vst;

std::string utf8(const TChar* s) {
    std::string out;
    for (int i = 0; i < 128 && s[i]; ++i) {
        std::uint32_t cp = s[i];
        if (cp >= 0xD800 && cp < 0xDC00 && s[i + 1] >= 0xDC00 && s[i + 1] < 0xE000) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (static_cast<std::uint32_t>(s[i + 1]) - 0xDC00);
            ++i;
        }
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
    return out;
}

// Accepts and ignores edit notifications (they must not recurse into the render path); records restart
// requests for the renderer (spec 05 section 9).
class Handler final : public IComponentHandler {
public:
    std::atomic<bool> restart{false}, rescan{false};

    tresult PLUGIN_API beginEdit(ParamID) override { return kResultOk; }
    tresult PLUGIN_API performEdit(ParamID, Steinberg::Vst::ParamValue) override { return kResultOk; }
    tresult PLUGIN_API endEdit(ParamID) override { return kResultOk; }
    tresult PLUGIN_API restartComponent(int32 flags) override {
        if (flags & (kReloadComponent | kLatencyChanged | kParamTitlesChanged)) restart = true;
        if (flags & (kReloadComponent | kParamTitlesChanged)) rescan = true;
        return kResultOk;
    }

    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
        if (FUnknownPrivate::iidEqual(iid, IComponentHandler::iid) || FUnknownPrivate::iidEqual(iid, FUnknown::iid)) {
            *obj = static_cast<IComponentHandler*>(this);
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return 1; }  // owned by the engine
    uint32 PLUGIN_API release() override { return 1; }
};

class Vst3Engine final : public Engine {
public:
    explicit Vst3Engine(std::shared_ptr<ThreadRoles> roles) : roles_(std::move(roles)) {}

    ~Vst3Engine() override {
        if (controller_) controller_->setComponentHandler(nullptr);
        data_.unprepare();
        // Every interface pointer goes before the module: one released after FreeLibrary calls into unloaded
        // code (Linux keeps a dlclose'd library mapped, so only Windows crashed).
        unit_info_ = nullptr;
        controller_ = nullptr;
        processor_ = nullptr;
        component_ = nullptr;
        provider_ = nullptr;  // terminates the plugin
        module_.reset();
    }

    expected<void, HostError> load(const std::filesystem::path& path, std::string_view uid) {
        std::string error;
        module_ = VST3::Hosting::Module::create(path.string(), error);
        if (!module_) return unexpected(HostError{std::format("'{}': {}", path.string(), error)});
        const auto& factory = module_->getFactory();
        std::optional<VST3::Hosting::ClassInfo> chosen;
        std::string ids;
        int count = 0;
        for (const auto& ci : factory.classInfos()) {
            if (ci.category() != kVstAudioEffectClass) continue;
            ++count;
            ids += (ids.empty() ? "" : ", ") + ci.ID().toString();
            if (!chosen && (uid.empty() || uid == ci.ID().toString() || uid == ci.name())) chosen = ci;
        }
        if (count == 0) return unexpected(HostError{"the module contains no audio processor class"});
        if (uid.empty() && count > 1)
            return unexpected(HostError{std::format("the module contains several plugins, pick one by uid: {}", ids)});
        if (!chosen) return unexpected(HostError{std::format("no plugin '{}' in the module (has: {})", uid, ids)});

        PluginContextFactory::instance().setPluginContext(&host_app_);
        provider_ = Steinberg::owned(new PlugProvider(factory, *chosen, true));
        if (!provider_->initialize()) return unexpected(HostError{"plugin initialisation failed"});
        component_ = provider_->getComponentPtr();
        controller_ = provider_->getControllerPtr();
        if (!component_) return unexpected(HostError{"no component"});
        processor_ = FUnknownPtr<IAudioProcessor>(component_);
        if (!processor_) return unexpected(HostError{"the component is not an audio processor"});
        if (controller_) controller_->setComponentHandler(&handler_);
        unit_info_ = controller_ ? FUnknownPtr<IUnitInfo>(controller_) : nullptr;
        // MIDI pitch bend goes in as the parameter the controller maps it to (spec 02 section 5). A parameter the
        // controller maps a MIDI controller to AND that is not automatable is a MIDI input proxy (clap-wrapper,
        // JUCE), not plugin state: it is left out of the parameter table, so a full vector never sends "CC 7 = 0"
        // and the table matches the CLAP build's. A real, automatable parameter a CC drives stays in.
        if (controller_) {
            FUnknownPtr<IMidiMapping> mapping(controller_);
            if (mapping) {
                ParamID id = 0;
                if (mapping->getMidiControllerAssignment(0, 0, ControllerNumbers::kPitchBend, id) == kResultTrue) bend_param_ = id;
                for (int16 ch = 0; ch < 16; ++ch)
                    for (int32 cc = 0; cc < kCountCtrlNumber; ++cc)
                        if (mapping->getMidiControllerAssignment(0, ch, CtrlNumber(cc), id) == kResultTrue) midi_params_.insert(id);
            }
        }

        info_.format = PluginFormat::Vst3;
        info_.uid = chosen->ID().toString();
        info_.name = chosen->name();
        info_.vendor = chosen->vendor();
        info_.version_hash = plugin_version_hash(path);
        const std::string sub = chosen->subCategoriesString();
        info_.kind = sub.find("Instrument") != std::string::npos ? PluginKind::Instrument
                     : sub.find("Fx") != std::string::npos      ? PluginKind::Effect
                                                                : PluginKind::Unknown;
        enumerate();
        return {};
    }

    const PluginInfo& info() const override { return info_; }
    const std::vector<ParamDesc>& parameters() const override { return params_list_; }

    std::string value_text(std::uint32_t id, double coord) const override {
        if (!controller_) return {};
        String128 text{};
        if (controller_->getParamStringByValue(id, std::clamp(coord, 0.0, 1.0), text) != kResultOk) return {};
        return utf8(text);
    }

    expected<void, HostError> activate(int sample_rate, int max_block) override {
        // Stereo main output; inputs keep the plugin's own arrangement.
        const int32 n_in = component_->getBusCount(kAudio, kInput);
        const int32 n_out = component_->getBusCount(kAudio, kOutput);
        if (n_out < 1) return unexpected(HostError{"the plugin has no audio output bus"});
        std::vector<SpeakerArrangement> ins(static_cast<std::size_t>(n_in)), outs(static_cast<std::size_t>(n_out));
        for (int32 i = 0; i < n_in; ++i) processor_->getBusArrangement(kInput, i, ins[static_cast<std::size_t>(i)]);
        for (int32 i = 0; i < n_out; ++i) processor_->getBusArrangement(kOutput, i, outs[static_cast<std::size_t>(i)]);
        outs[0] = SpeakerArr::kStereo;
        processor_->setBusArrangements(ins.empty() ? nullptr : ins.data(), n_in, outs.data(), n_out);
        SpeakerArrangement main_out = 0;
        processor_->getBusArrangement(kOutput, 0, main_out);
        out_channels_ = std::max(SpeakerArr::getChannelCount(main_out), 1);

        ProcessSetup setup{kOffline, kSample32, max_block, static_cast<SampleRate>(sample_rate)};
        if (processor_->setupProcessing(setup) != kResultOk)
            return unexpected(HostError{"setupProcessing failed"});
        for (int32 i = 0; i < component_->getBusCount(kAudio, kInput); ++i) component_->activateBus(kAudio, kInput, i, true);
        for (int32 i = 0; i < component_->getBusCount(kAudio, kOutput); ++i) component_->activateBus(kAudio, kOutput, i, true);
        for (int32 i = 0; i < component_->getBusCount(kEvent, kInput); ++i) component_->activateBus(kEvent, kInput, i, true);
        if (component_->setActive(true) != kResultOk) return unexpected(HostError{"setActive(true) failed"});

        data_.unprepare();
        if (!data_.prepare(*component_, max_block, kSample32)) {
            component_->setActive(false);
            return unexpected(HostError{"cannot allocate process buffers"});
        }
        rate_ = sample_rate;
        max_block_ = max_block;
        position_ = 0;
        active_ = true;
        return {};
    }

    void deactivate() override {
        if (active_) component_->setActive(false);
        active_ = false;
    }

    expected<std::vector<std::uint8_t>, HostError> save_state() override {
        auto write = [](auto&& fn) -> expected<std::vector<std::uint8_t>, HostError> {
            MemoryStream stream;
            if (fn(&stream) != kResultOk) return unexpected(HostError{"state save failed"});
            const auto* p = reinterpret_cast<const std::uint8_t*>(stream.getData());
            return std::vector<std::uint8_t>(p, p + stream.getSize());
        };
        auto comp = write([&](IBStream* s) { return component_->getState(s); });
        if (!comp) return comp;
        std::vector<std::uint8_t> ctrl;
        if (controller_) {
            auto c = write([&](IBStream* s) { return controller_->getState(s); });
            if (c) ctrl = std::move(*c);  // a controller without state is fine
        }
        std::vector<std::uint8_t> blob;
        for (const auto* part : {&*comp, &ctrl}) {
            const auto n = static_cast<std::uint32_t>(part->size());
            for (int i = 0; i < 4; ++i) blob.push_back(static_cast<std::uint8_t>(n >> (8 * i)));
            blob.insert(blob.end(), part->begin(), part->end());
        }
        return blob;
    }

    expected<void, HostError> load_state(const std::vector<std::uint8_t>& blob) override {
        std::size_t pos = 0;
        auto take = [&](std::vector<std::uint8_t>& out) -> bool {
            if (pos + 4 > blob.size()) return false;
            std::uint32_t n = 0;
            for (int i = 0; i < 4; ++i) n |= static_cast<std::uint32_t>(blob[pos + static_cast<std::size_t>(i)]) << (8 * i);
            pos += 4;
            if (pos + n > blob.size()) return false;
            out.assign(blob.begin() + static_cast<std::ptrdiff_t>(pos), blob.begin() + static_cast<std::ptrdiff_t>(pos + n));
            pos += n;
            return true;
        };
        std::vector<std::uint8_t> comp, ctrl;
        if (!take(comp) || !take(ctrl)) return unexpected(HostError{"state blob is malformed"});
        {
            MemoryStream s(comp.data(), static_cast<TSize>(comp.size()));
            if (component_->setState(&s) != kResultOk) return unexpected(HostError{"component setState failed"});
            if (controller_) {
                MemoryStream again(comp.data(), static_cast<TSize>(comp.size()));
                controller_->setComponentState(&again);
            }
        }
        if (controller_ && !ctrl.empty()) {
            MemoryStream s(ctrl.data(), static_cast<TSize>(ctrl.size()));
            controller_->setState(&s);
        }
        return {};
    }

    bool rescan_parameters() override {
        if (!handler_.rescan.exchange(false)) return false;
        const auto before = params_list_;
        enumerate();
        if (before.size() != params_list_.size()) return true;
        for (std::size_t i = 0; i < before.size(); ++i)
            if (before[i].id != params_list_[i].id || before[i].name != params_list_[i].name) return true;
        return false;
    }

    bool take_restart_request() override { return handler_.restart.exchange(false); }

    void apply_controller_values(std::span<const mimicry::host::ParamValue> values) override {
        if (!controller_) return;
        for (const auto& v : values) controller_->setParamNormalized(v.id, v.coord);
    }

    expected<void, HostError> start() override {
        if (processor_->setProcessing(true) != kResultOk && processor_->setProcessing(true) != kNotImplemented)
            return unexpected(HostError{"setProcessing(true) failed"});
        return {};
    }

    void stop() override {
        if (processor_) processor_->setProcessing(false);
    }

    expected<void, HostError> process(const Block& block, float* left, float* right) override {
        if (block.frames > max_block_) return unexpected(HostError{"block larger than the activated maximum"});
        const int frames = block.frames;

        changes_.clearQueue();
        for (const auto& pv : block.params) {
            int32 index = 0;
            if (auto* q = changes_.addParameterData(pv.id, index)) q->addPoint(0, std::clamp(pv.coord, 0.0, 1.0), index);
        }
        events_.clear();
        for (const NoteEvent& n : block.notes) {
            if (n.set_bend && bend_param_) {  // normalised bend: 0.5 is centre
                int32 index = 0;
                if (auto* q = changes_.addParameterData(*bend_param_, index))
                    q->addPoint(static_cast<int32>(n.frame), std::clamp(0.5 + 0.5 * n.bend, 0.0, 1.0), index);
            }
            Event e{};
            e.busIndex = 0;
            e.sampleOffset = static_cast<int32>(n.frame);
            e.ppqPosition = 0;
            e.flags = 0;
            if (n.on) {
                e.type = Event::kNoteOnEvent;
                e.noteOn.channel = 0;
                e.noteOn.pitch = static_cast<int16>(n.key);
                e.noteOn.tuning = static_cast<float>(n.cents);
                e.noteOn.velocity = n.velocity;
                e.noteOn.length = 0;
                e.noteOn.noteId = n.note_id;
            } else {
                e.type = Event::kNoteOffEvent;
                e.noteOff.channel = 0;
                e.noteOff.pitch = static_cast<int16>(n.key);
                e.noteOff.velocity = 0;
                e.noteOff.noteId = n.note_id;
                e.noteOff.tuning = 0;
            }
            events_.addEvent(e);
        }

        ProcessContext ctx{};
        ctx.state = ProcessContext::kPlaying | ProcessContext::kTempoValid | ProcessContext::kTimeSigValid |
                    ProcessContext::kProjectTimeMusicValid;
        ctx.sampleRate = rate_;
        ctx.projectTimeSamples = position_;
        ctx.tempo = 120.0;
        ctx.timeSigNumerator = 4;
        ctx.timeSigDenominator = 4;
        ctx.projectTimeMusic = static_cast<double>(position_) / rate_ * 2.0;

        data_.processMode = kOffline;
        data_.symbolicSampleSize = kSample32;
        data_.numSamples = frames;
        data_.inputParameterChanges = &changes_;
        data_.outputParameterChanges = &out_changes_;
        data_.inputEvents = &events_;
        data_.outputEvents = nullptr;
        data_.processContext = &ctx;
        out_changes_.clearQueue();
        if (data_.numOutputs > 0)
            for (int32 c = 0; c < data_.outputs[0].numChannels; ++c)
                std::fill_n(data_.outputs[0].channelBuffers32[c], frames, 0.0f);
        for (int32 b = 0; b < data_.numInputs; ++b)
            for (int32 c = 0; c < data_.inputs[b].numChannels; ++c)
                std::fill_n(data_.inputs[b].channelBuffers32[c], frames, 0.0f);

        const tresult r = processor_->process(data_);
        position_ += frames;
        if (r != kResultOk && r != kResultTrue) return unexpected(HostError{std::format("process returned {}", static_cast<int>(r))});

        const AudioBusBuffers& out = data_.outputs[0];
        const float* l = out.channelBuffers32[0];
        const float* rr = out.numChannels > 1 ? out.channelBuffers32[1] : l;
        std::copy_n(l, frames, left);
        std::copy_n(rr, frames, right);
        return {};
    }

    int tail_samples() const override {
        const uint32 t = processor_->getTailSamples();
        return t >= static_cast<uint32>(INT32_MAX) ? INT32_MAX : static_cast<int>(t);
    }
    int latency_samples() const override { return static_cast<int>(processor_->getLatencySamples()); }

private:
    std::string unit_path(UnitID id) const {
        std::string path;
        for (int guard = 0; unit_info_ && id != kRootUnitId && guard < 16; ++guard) {
            UnitInfo ui{};
            if (unit_info_->getUnitInfo(0, ui) == kResultOk) {}  // keep the interface warm for plugins that need it
            bool found = false;
            for (int32 i = 0; i < unit_info_->getUnitCount(); ++i) {
                if (unit_info_->getUnitInfo(i, ui) == kResultOk && ui.id == id) {
                    path = utf8(ui.name) + (path.empty() ? "" : "/" + path);
                    id = ui.parentUnitId;
                    found = true;
                    break;
                }
            }
            if (!found) break;
        }
        return path;
    }

    void enumerate() {
        params_list_.clear();
        index_.clear();
        if (!controller_) return;
        const int32 n = controller_->getParameterCount();
        for (int32 i = 0; i < n; ++i) {
            ParameterInfo pi{};
            if (controller_->getParameterInfo(i, pi) != kResultOk) continue;
            // MIDI input proxy (see open). IMidiMapping cannot be asked for program change (kCtrlProgramChange is
            // outside kCountCtrlNumber): a wrapper's program-change parameter stays, pinned and never sent.
            if (midi_params_.count(pi.id) && !(pi.flags & ParameterInfo::kCanAutomate)) continue;
            ParamDesc d;
            d.id = pi.id;
            d.name = utf8(pi.title);
            d.module = unit_path(pi.unitId);
            d.min = 0;
            d.max = 1;
            d.default_plain = pi.defaultNormalizedValue;
            d.default_coord = pi.defaultNormalizedValue;
            d.steps = pi.stepCount > 0 ? pi.stepCount + 1 : 0;
            d.log_scale = false;  // the plugin's own taper is kept (spec 02 section 1)
            d.flags.hidden = (pi.flags & ParameterInfo::kIsHidden) != 0;
            d.flags.readonly = (pi.flags & ParameterInfo::kIsReadOnly) != 0;
            d.flags.bypass = (pi.flags & ParameterInfo::kIsBypass) != 0;
            d.flags.program_change = (pi.flags & ParameterInfo::kIsProgramChange) != 0 ||
                                     (pi.stepCount > 0 && looks_like_program_param(d.name));
            d.flags.automatable = (pi.flags & ParameterInfo::kCanAutomate) != 0;
            for (int k = 0; k < 5; ++k) d.texts[static_cast<std::size_t>(k)] = value_text(d.id, k * 0.25);
            d.units = utf8(pi.units);
            if (d.units.empty()) d.units = units_from_texts(d.texts);
            index_[d.id] = params_list_.size();
            params_list_.push_back(std::move(d));
        }
    }

    std::shared_ptr<ThreadRoles> roles_;
    VST3::Hosting::Module::Ptr module_;
    HostApplication host_app_;
    Handler handler_;
    IPtr<PlugProvider> provider_;
    IPtr<IComponent> component_;
    IPtr<IEditController> controller_;
    FUnknownPtr<IAudioProcessor> processor_;
    FUnknownPtr<IUnitInfo> unit_info_;
    std::optional<ParamID> bend_param_;  // IMidiMapping of kPitchBend, if the controller offers one
    std::unordered_set<ParamID> midi_params_;  // every IMidiMapping target

    PluginInfo info_;
    std::vector<ParamDesc> params_list_;
    std::unordered_map<std::uint32_t, std::size_t> index_;

    HostProcessData data_;
    ParameterChanges changes_, out_changes_;
    EventList events_{64};
    int rate_ = 48000, max_block_ = 512, out_channels_ = 2;
    std::int64_t position_ = 0;
    bool active_ = false;
};

}  // namespace

expected<std::unique_ptr<Engine>, HostError> open_vst3_engine(const std::filesystem::path& path, std::string_view uid,
                                                              std::shared_ptr<ThreadRoles> roles) {
    auto engine = std::make_unique<Vst3Engine>(std::move(roles));
    if (auto r = engine->load(path, uid); !r) return unexpected(r.error());
    return std::unique_ptr<Engine>(std::move(engine));
}

}  // namespace mimicry::host
