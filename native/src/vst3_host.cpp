// vst3_host.cpp — NetPlugHost native VST3 host, ABI v1.
//
// Implements the flat C ABI in include/vst3_host_c.h (ABI_SPEC.md §3) with the
// semantics in §6. No C++ type crosses the boundary; every entry point is wrapped
// so no exception escapes (§2).

#include "vst3_host_c.h"

#include "public.sdk/source/vst/hosting/module.h"
#include "public.sdk/source/vst/hosting/plugprovider.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/hosting/parameterchanges.h"
#include "public.sdk/source/vst/utility/stringconvert.h"
#include "public.sdk/source/common/memorystream.h"

#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/base/funknownimpl.h"

#if SMTG_OS_WINDOWS
// For SetWindowPos in PlugFrame::resizeView. NOMINMAX keeps the Windows min/max macros
// from colliding with std::min / std::max, and WIN32_LEAN_AND_MEAN trims the header.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

using namespace Steinberg;
using namespace Steinberg::Vst;

// The SDK declares IPlugFrame's IID but defines it in none of the translation units this
// target compiles (neither coreiids.cpp nor vstinitiids.cpp carries it), leaving it to the
// host. Implementing IPlugFrame needs the IID for queryInterface, so define it here.
namespace Steinberg {
DEF_CLASS_IID(IPlugFrame)
}

namespace {

// ---------------------------------------------------------------------------
// Handle validation (§4: VST3_ERR_INVALID_HANDLE covers "not one this DLL issued")
// ---------------------------------------------------------------------------
constexpr uint32_t kModuleMagic = 0x4E504D30u;  // "NPM0"
constexpr uint32_t kPluginMagic = 0x4E505030u;  // "NPP0"

std::mutex g_registryMutex;
std::unordered_set<void*> g_liveModules;
std::unordered_set<void*> g_livePlugins;

void registerHandle(std::unordered_set<void*>& set, void* h) {
    std::lock_guard<std::mutex> lock(g_registryMutex);
    set.insert(h);
}
void unregisterHandle(std::unordered_set<void*>& set, void* h) {
    std::lock_guard<std::mutex> lock(g_registryMutex);
    set.erase(h);
}
bool isRegistered(std::unordered_set<void*>& set, void* h) {
    std::lock_guard<std::mutex> lock(g_registryMutex);
    return set.find(h) != set.end();
}

// ---------------------------------------------------------------------------
// UTF-16 fixed-array string copy. Truncates to fit, always null-terminates (§5).
// ---------------------------------------------------------------------------
void copyU16(char16_t* dest, size_t capacity, const std::u16string& src) {
    if (!dest || capacity == 0) return;
    const size_t n = src.size() < (capacity - 1) ? src.size() : (capacity - 1);
    if (n) std::memcpy(dest, src.data(), n * sizeof(char16_t));
    dest[n] = u'\0';
}
void copyU16(char16_t* dest, size_t capacity, const std::string& utf8) {
    copyU16(dest, capacity, Steinberg::Vst::StringConvert::convert(utf8));
}

// ---------------------------------------------------------------------------
// Host application singleton. The plugin context must outlive every plugin.
// ---------------------------------------------------------------------------
void ensureHostContext() {
    static std::once_flag once;
    std::call_once(once, [] {
        static auto host = owned(new HostApplication());
        PluginContextFactory::instance().setPluginContext(host);
    });
}

// ---------------------------------------------------------------------------
// Lock-free SPSC parameter queue (§6.1: set_param is the one cross-thread call).
// Single producer (UI thread / component handler), single consumer (audio thread).
// Fixed capacity, no allocation on either side. Oldest entries are dropped if the
// audio thread stalls long enough to let the ring fill — a dropped intermediate
// knob position is preferable to blocking the render thread.
// ---------------------------------------------------------------------------
class ParamQueue {
public:
    struct Entry {
        ParamID id;
        double  value;
    };

    void push(ParamID id, double value) {
        const size_t w = writePos_.load(std::memory_order_relaxed);
        const size_t next = (w + 1) & kMask;
        if (next == readPos_.load(std::memory_order_acquire)) {
            // Full: drop the oldest so the newest (most current) value survives.
            readPos_.store((readPos_.load(std::memory_order_relaxed) + 1) & kMask,
                           std::memory_order_release);
        }
        entries_[w].id = id;
        entries_[w].value = value;
        writePos_.store(next, std::memory_order_release);
    }

    bool pop(Entry& out) {
        const size_t r = readPos_.load(std::memory_order_relaxed);
        if (r == writePos_.load(std::memory_order_acquire)) return false;
        out = entries_[r];
        readPos_.store((r + 1) & kMask, std::memory_order_release);
        return true;
    }

private:
    static constexpr size_t kCapacity = 1024;  // power of two
    static constexpr size_t kMask = kCapacity - 1;
    Entry entries_[kCapacity]{};
    std::atomic<size_t> writePos_{0};
    std::atomic<size_t> readPos_{0};
};

class PluginInstance;

// ---------------------------------------------------------------------------
// Minimal IComponentHandler. Editor-driven parameter edits land in the same queue
// as vst3_plugin_set_param, so the processor hears knob moves made in the plugin UI.
// ---------------------------------------------------------------------------
class ComponentHandler : public U::ImplementsNonDestroyable<U::Directly<IComponentHandler>> {
public:
    explicit ComponentHandler(ParamQueue& queue) : queue_(queue) {}

    tresult PLUGIN_API beginEdit(ParamID) SMTG_OVERRIDE { return kResultOk; }
    tresult PLUGIN_API performEdit(ParamID id, ParamValue valueNormalized) SMTG_OVERRIDE {
        queue_.push(id, valueNormalized);
        return kResultOk;
    }
    tresult PLUGIN_API endEdit(ParamID) SMTG_OVERRIDE { return kResultOk; }
    tresult PLUGIN_API restartComponent(int32) SMTG_OVERRIDE { return kResultOk; }

private:
    ParamQueue& queue_;
};

// ---------------------------------------------------------------------------
// Plug frame
// ---------------------------------------------------------------------------
// The host side of IPlugView. The spec requires setFrame() before attached(), and
// plug-ins are entitled to call plugFrame->resizeView() from inside attached() when
// their editor sizes or scales itself to the host.
//
// This was previously left null, which is not a benign omission: a plug-in that does
// not null-check its frame dereferences null and takes the host process down with it.
// Native Instruments' plug-ins do exactly this -- Battery 4 and Kontakt died inside
// attached(), and Super 8 produced a bare 0xC0000005 access violation -- while the same
// plug-ins load correctly in any DAW, all of which implement this interface.
//
// resizeView() must resize the parent window and return kResultTrue. Returning
// kResultFalse tells the plug-in the host refused, and plug-ins that insist on their
// requested size may then refuse to draw at all.
class PlugFrame : public U::ImplementsNonDestroyable<U::Directly<IPlugFrame>> {
public:
    tresult PLUGIN_API resizeView(IPlugView* view, ViewRect* newSize) SMTG_OVERRIDE {
        if (!view || !newSize) return kInvalidArgument;

#if SMTG_OS_WINDOWS
        // Resize the window the view was attached to, then let the view match it. Both
        // steps are required: the window is ours to size, the view's layout is its own.
        if (parent_) {
            ::SetWindowPos(static_cast<HWND>(parent_), nullptr, 0, 0,
                           newSize->getWidth(), newSize->getHeight(),
                           SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
#endif
        view->onSize(newSize);
        return kResultTrue;
    }

    // The window the view is attached to, so resizeView has something to resize.
    void setParent(void* parent) { parent_ = parent; }

private:
    void* parent_ = nullptr;
};

// ---------------------------------------------------------------------------
// Module
// ---------------------------------------------------------------------------
struct ModuleInstance {
    uint32_t magic = kModuleMagic;
    VST3::Hosting::Module::Ptr module;
    // Audio-effect classes only, in factory order. Indices in the ABI address THIS
    // list (§3 class_count is "number of audio-effect classes"), so class_info[i]
    // always reports isAudioEffect == 1 (§8 step 1).
    std::vector<VST3::Hosting::ClassInfo> classes;
};

ModuleInstance* asModule(Vst3ModuleHandle h) {
    if (!h) return nullptr;
    auto* m = static_cast<ModuleInstance*>(h);
    if (!isRegistered(g_liveModules, h) || m->magic != kModuleMagic) return nullptr;
    return m;
}

// ---------------------------------------------------------------------------
// Plugin
// ---------------------------------------------------------------------------
class PluginInstance {
public:
    uint32_t magic = kPluginMagic;

    IPtr<PlugProvider>   provider;
    IPtr<IComponent>     component;
    IPtr<IEditController> controller;
    IPtr<IAudioProcessor> processor;
    IPtr<IPlugView>      view;

    ParamQueue       paramQueue;
    ComponentHandler handler{paramQueue};
    PlugFrame        plugFrame;

    bool    isSetup = false;
    bool    isActive = false;
    int32_t maxBlockSize = 0;
    int32_t numInputs = 0;
    int32_t numOutputs = 0;

    // Persistent process scaffolding — allocated at setup, never on the audio thread.
    // One AudioBusBuffers per bus the plugin declares, not just the main one: many
    // effects (compressors especially) declare an auxiliary side-chain input, and they
    // reject a partial arrangement and index every declared bus during process.
    // Side-chain routing itself stays out of scope (§9) — aux buses are negotiated as
    // empty, deactivated, and passed through as zero-channel buffers.
    ParameterChanges inputChanges;
    std::vector<AudioBusBuffers> inputBuses;
    std::vector<AudioBusBuffers> outputBuses;

    // The host's OWN channel-pointer arrays. These must not be the caller's arrays:
    // plugins may legitimately mutate channelBuffers32 in place while processing (the
    // SDK's ProcessDataSlicer advances every pointer per slice and reverts at the end).
    // Pointing both buses at a caller array would corrupt the caller's memory, and when
    // the caller processes in-place — which the ABI explicitly allows — both buses would
    // alias one array and get advanced twice per slice, running off the end of the buffers.
    std::vector<float*> inputPtrs;
    std::vector<float*> outputPtrs;

    ~PluginInstance() {
        closeEditor();
        if (isActive && component) {
            if (processor) processor->setProcessing(false);
            component->setActive(false);
        }
        if (controller) controller->setComponentHandler(nullptr);
        processor.reset();
        // provider's destructor disconnects and terminates component + controller.
        controller.reset();
        component.reset();
        provider.reset();
    }

    Vst3Status closeEditor() {
        if (view) {
            view->setFrame(nullptr);
            view->removed();
            view = nullptr;
        }
        // The HWND is the caller's and may be destroyed the moment we return.
        plugFrame.setParent(nullptr);
        return VST3_OK;
    }

    // Drain queued param changes into the VST3 input change list. Audio thread.
    void drainParams() {
        inputChanges.clearQueue();
        ParamQueue::Entry e{};
        while (paramQueue.pop(e)) {
            int32 queueIndex = 0;
            if (auto* q = inputChanges.addParameterData(e.id, queueIndex)) {
                int32 pointIndex = 0;
                q->addPoint(0, e.value, pointIndex);
            }
        }
    }
};

PluginInstance* asPlugin(Vst3PluginHandle h) {
    if (!h) return nullptr;
    auto* p = static_cast<PluginInstance*>(h);
    if (!isRegistered(g_livePlugins, h) || p->magic != kPluginMagic) return nullptr;
    return p;
}

SpeakerArrangement arrangementFor(int32_t channels) {
    switch (channels) {
        case 1:  return SpeakerArr::kMono;
        case 2:  return SpeakerArr::kStereo;
        default: return 0;  // §6.4/§9: >2 channels is out of scope for v1
    }
}

int32_t paramFlagsFrom(int32 vstFlags) {
    int32_t flags = 0;
    if (vstFlags & ParameterInfo::kCanAutomate) flags |= VST3_PARAM_CAN_AUTOMATE;
    if (vstFlags & ParameterInfo::kIsBypass)    flags |= VST3_PARAM_IS_BYPASS;
    if (vstFlags & ParameterInfo::kIsReadOnly)  flags |= VST3_PARAM_IS_READ_ONLY;
    if (vstFlags & ParameterInfo::kIsList)      flags |= VST3_PARAM_IS_LIST;
    return flags;
}

}  // namespace

// ---------------------------------------------------------------------------
// Exception firewall (§2: a C++ exception reaching the ABI edge is a bug).
// ---------------------------------------------------------------------------
#define VST3_GUARD_BEGIN try {
#define VST3_GUARD_END(failValue) \
    } catch (...) { return (failValue); }
// Void entry points have no status to return; swallowing is the only option, and
// these are all teardown paths where there is nothing left to report to.
#define VST3_GUARD_END_VOID \
    } catch (...) { }

// ===========================================================================
// Module lifecycle
// ===========================================================================

Vst3Status __cdecl vst3_module_load(const wchar_t* path, Vst3ModuleHandle* outModule) {
    VST3_GUARD_BEGIN
    if (!path || !outModule) return VST3_ERR_INVALID_ARG;
    *outModule = nullptr;

    ensureHostContext();

    // The SDK's module loader is UTF-8; the ABI is UTF-16 (§2).
    const std::u16string wide(reinterpret_cast<const char16_t*>(path));
    const std::string utf8 = Steinberg::Vst::StringConvert::convert(wide);

    std::string error;
    auto module = VST3::Hosting::Module::create(utf8, error);
    if (!module) return VST3_ERR_LOAD_FAILED;

    auto instance = std::make_unique<ModuleInstance>();
    instance->module = module;

    for (const auto& ci : module->getFactory().classInfos()) {
        if (ci.category() == kVstAudioEffectClass) instance->classes.push_back(ci);
    }

    ModuleInstance* raw = instance.release();
    registerHandle(g_liveModules, raw);
    *outModule = raw;
    return VST3_OK;
    VST3_GUARD_END(VST3_ERR_INTERNAL)
}

int32_t __cdecl vst3_module_class_count(Vst3ModuleHandle module) {
    VST3_GUARD_BEGIN
    auto* m = asModule(module);
    if (!m) return VST3_ERR_INVALID_HANDLE;
    return static_cast<int32_t>(m->classes.size());
    VST3_GUARD_END(VST3_ERR_INTERNAL)
}

Vst3Status __cdecl vst3_module_class_info(Vst3ModuleHandle module, int32_t index, Vst3ClassInfo* outInfo) {
    VST3_GUARD_BEGIN
    auto* m = asModule(module);
    if (!m) return VST3_ERR_INVALID_HANDLE;
    if (!outInfo || index < 0 || index >= static_cast<int32_t>(m->classes.size()))
        return VST3_ERR_INVALID_ARG;

    const auto& ci = m->classes[static_cast<size_t>(index)];
    std::memset(outInfo, 0, sizeof(*outInfo));
    copyU16(outInfo->name, 128, ci.name());
    copyU16(outInfo->category, 128, ci.subCategoriesString());
    copyU16(outInfo->vendor, 128, ci.vendor());
    copyU16(outInfo->version, 64, ci.version());
    outInfo->isAudioEffect = 1;  // the list is pre-filtered to audio-effect classes
    return VST3_OK;
    VST3_GUARD_END(VST3_ERR_INTERNAL)
}

void __cdecl vst3_module_free(Vst3ModuleHandle module) {
    VST3_GUARD_BEGIN
    auto* m = asModule(module);
    if (!m) return;
    unregisterHandle(g_liveModules, module);
    m->magic = 0;
    delete m;
    VST3_GUARD_END_VOID
}

// ===========================================================================
// Plugin lifecycle
// ===========================================================================

Vst3Status __cdecl vst3_plugin_create(Vst3ModuleHandle module, int32_t classIndex, Vst3PluginHandle* outPlugin) {
    VST3_GUARD_BEGIN
    auto* m = asModule(module);
    if (!m) return VST3_ERR_INVALID_HANDLE;
    if (!outPlugin) return VST3_ERR_INVALID_ARG;
    *outPlugin = nullptr;

    if (m->classes.empty()) return VST3_ERR_NO_AUDIO_CLASS;
    if (classIndex < 0 || classIndex >= static_cast<int32_t>(m->classes.size()))
        return VST3_ERR_NO_AUDIO_CLASS;  // §4: create with a bad index

    ensureHostContext();

    auto plugin = std::make_unique<PluginInstance>();
    plugin->provider = owned(new PlugProvider(m->module->getFactory(),
                                              m->classes[static_cast<size_t>(classIndex)],
                                              true));
    if (!plugin->provider->initialize()) return VST3_ERR_LOAD_FAILED;

    plugin->component = plugin->provider->getComponentPtr();
    plugin->controller = plugin->provider->getControllerPtr();
    if (!plugin->component) return VST3_ERR_LOAD_FAILED;

    plugin->processor = FUnknownPtr<IAudioProcessor>(plugin->component);
    if (!plugin->processor) return VST3_ERR_NO_AUDIO_CLASS;

    if (plugin->controller)
        plugin->controller->setComponentHandler(&plugin->handler);

    PluginInstance* raw = plugin.release();
    registerHandle(g_livePlugins, raw);
    *outPlugin = raw;
    return VST3_OK;
    VST3_GUARD_END(VST3_ERR_INTERNAL)
}

void __cdecl vst3_plugin_destroy(Vst3PluginHandle plugin) {
    VST3_GUARD_BEGIN
    auto* p = asPlugin(plugin);
    if (!p) return;
    unregisterHandle(g_livePlugins, plugin);
    p->magic = 0;
    delete p;
    VST3_GUARD_END_VOID
}

// ===========================================================================
// Processing setup
// ===========================================================================

Vst3Status __cdecl vst3_plugin_setup(Vst3PluginHandle plugin, double sampleRate, int32_t maxBlockSize,
                                     int32_t inputChannels, int32_t outputChannels) {
    VST3_GUARD_BEGIN
    auto* p = asPlugin(plugin);
    if (!p) return VST3_ERR_INVALID_HANDLE;
    if (sampleRate <= 0.0 || maxBlockSize <= 0) return VST3_ERR_INVALID_ARG;

    const SpeakerArrangement inArr = arrangementFor(inputChannels);
    const SpeakerArrangement outArr = arrangementFor(outputChannels);
    if (inArr == 0 || outArr == 0) return VST3_ERR_INVALID_ARG;

    // Re-preparing is allowed (§3): drop back to inactive first.
    if (p->isActive) {
        p->processor->setProcessing(false);
        p->component->setActive(false);
        p->isActive = false;
    }
    p->isSetup = false;

    const int32 numInBuses = p->component->getBusCount(kAudio, kInput);
    const int32 numOutBuses = p->component->getBusCount(kAudio, kOutput);
    if (numInBuses < 1 || numOutBuses < 1) return VST3_ERR_UNSUPPORTED_IO;

    // Negotiate every declared bus in one call: the main bus gets the requested
    // arrangement, auxiliary buses get kEmpty. Passing arrangements for only the main
    // bus makes plugins with a side-chain reject the whole request.
    std::vector<SpeakerArrangement> ins(static_cast<size_t>(numInBuses), SpeakerArr::kEmpty);
    std::vector<SpeakerArrangement> outs(static_cast<size_t>(numOutBuses), SpeakerArr::kEmpty);
    ins[0] = inArr;
    outs[0] = outArr;

    // A kResultFalse here is not decisive: plenty of plugins refuse to be *told* an
    // arrangement while already being in exactly the one requested. What matters for
    // §6.4 is the arrangement actually in effect, so the answer below is authoritative
    // and this return value is only a hint.
    p->processor->setBusArrangements(ins.data(), numInBuses, outs.data(), numOutBuses);

    // §6.4: the main bus must end up exactly as requested — never a silent substitution.
    SpeakerArrangement actualIn = 0, actualOut = 0;
    if (p->processor->getBusArrangement(kInput, 0, actualIn) != kResultTrue ||
        p->processor->getBusArrangement(kOutput, 0, actualOut) != kResultTrue)
        return VST3_ERR_UNSUPPORTED_IO;
    if (actualIn != inArr || actualOut != outArr)
        return VST3_ERR_UNSUPPORTED_IO;

    ProcessSetup setup{};
    setup.processMode = kRealtime;
    setup.symbolicSampleSize = kSample32;
    setup.maxSamplesPerBlock = maxBlockSize;
    setup.sampleRate = sampleRate;
    if (p->processor->setupProcessing(setup) != kResultTrue) return VST3_ERR_UNSUPPORTED_IO;

    // Main bus on, auxiliary buses off (a failure to deactivate an aux bus is not fatal).
    if (p->component->activateBus(kAudio, kInput, 0, true) != kResultTrue)
        return VST3_ERR_UNSUPPORTED_IO;
    if (p->component->activateBus(kAudio, kOutput, 0, true) != kResultTrue)
        return VST3_ERR_UNSUPPORTED_IO;
    for (int32 b = 1; b < numInBuses; ++b) p->component->activateBus(kAudio, kInput, b, false);
    for (int32 b = 1; b < numOutBuses; ++b) p->component->activateBus(kAudio, kOutput, b, false);

    p->maxBlockSize = maxBlockSize;
    p->numInputs = inputChannels;
    p->numOutputs = outputChannels;

    // Sized here so process() never allocates (§6.1). Aux buses stay zero-channel.
    p->inputPtrs.assign(static_cast<size_t>(inputChannels), nullptr);
    p->outputPtrs.assign(static_cast<size_t>(outputChannels), nullptr);

    p->inputBuses.assign(static_cast<size_t>(numInBuses), AudioBusBuffers{});
    p->outputBuses.assign(static_cast<size_t>(numOutBuses), AudioBusBuffers{});
    p->inputBuses[0].numChannels = inputChannels;
    p->inputBuses[0].channelBuffers32 = p->inputPtrs.data();
    p->outputBuses[0].numChannels = outputChannels;
    p->outputBuses[0].channelBuffers32 = p->outputPtrs.data();

    p->isSetup = true;
    return VST3_OK;
    VST3_GUARD_END(VST3_ERR_INTERNAL)
}

Vst3Status __cdecl vst3_plugin_set_active(Vst3PluginHandle plugin, int32_t active) {
    VST3_GUARD_BEGIN
    auto* p = asPlugin(plugin);
    if (!p) return VST3_ERR_INVALID_HANDLE;
    if (!p->isSetup) return VST3_ERR_NOT_SETUP;

    if (active != 0) {
        if (p->isActive) return VST3_OK;
        if (p->component->setActive(true) != kResultTrue) return VST3_ERR_INTERNAL;
        if (p->processor->setProcessing(true) != kResultTrue) {
            // Not every plugin implements setProcessing; an error here is not fatal
            // as long as the component activated.
        }
        p->isActive = true;
    } else {
        if (!p->isActive) return VST3_OK;
        p->processor->setProcessing(false);
        if (p->component->setActive(false) != kResultTrue) return VST3_ERR_INTERNAL;
        p->isActive = false;
    }
    return VST3_OK;
    VST3_GUARD_END(VST3_ERR_INTERNAL)
}

int32_t __cdecl vst3_plugin_latency_samples(Vst3PluginHandle plugin) {
    VST3_GUARD_BEGIN
    auto* p = asPlugin(plugin);
    if (!p) return VST3_ERR_INVALID_HANDLE;
    if (!p->processor) return VST3_ERR_INVALID_HANDLE;
    const uint32 latency = p->processor->getLatencySamples();
    return static_cast<int32_t>(latency);
    VST3_GUARD_END(VST3_ERR_INTERNAL)
}

Vst3Status __cdecl vst3_plugin_reset(Vst3PluginHandle plugin) {
    VST3_GUARD_BEGIN
    auto* p = asPlugin(plugin);
    if (!p) return VST3_ERR_INVALID_HANDLE;
    if (!p->isSetup) return VST3_ERR_NOT_SETUP;
    if (!p->isActive) return VST3_OK;  // nothing buffered to flush

    // §3: setProcessing(false)+setProcessing(true) — no reallocation.
    p->processor->setProcessing(false);
    p->processor->setProcessing(true);
    return VST3_OK;
    VST3_GUARD_END(VST3_ERR_INTERNAL)
}

// ===========================================================================
// Processing (audio thread)
// ===========================================================================

Vst3Status __cdecl vst3_plugin_process(Vst3PluginHandle plugin,
                                       const float* const* inputs,
                                       float* const* outputs,
                                       int32_t frames) {
    VST3_GUARD_BEGIN
    auto* p = asPlugin(plugin);
    if (!p) return VST3_ERR_INVALID_HANDLE;
    if (!p->isSetup || !p->isActive) return VST3_ERR_NOT_SETUP;
    if (!inputs || !outputs || frames < 0) return VST3_ERR_INVALID_ARG;
    if (frames > p->maxBlockSize) return VST3_ERR_INVALID_ARG;  // §6.5
    if (frames == 0) return VST3_OK;

    p->drainParams();

    // Copy the caller's channel pointers into our own arrays rather than handing the
    // plugin the caller's memory to write through. In-place processing is allowed, so
    // `inputs` and `outputs` may be the very same array — these two copies keep the
    // buses independent. Bounded, allocation-free work.
    for (int32_t c = 0; c < p->numInputs; ++c)
        p->inputPtrs[static_cast<size_t>(c)] = const_cast<float*>(inputs[c]);
    for (int32_t c = 0; c < p->numOutputs; ++c)
        p->outputPtrs[static_cast<size_t>(c)] = outputs[c];

    // The slicer may advance these per slice, so reset them every block.
    p->inputBuses[0].channelBuffers32 = p->inputPtrs.data();
    p->outputBuses[0].channelBuffers32 = p->outputPtrs.data();
    p->inputBuses[0].silenceFlags = 0;
    p->outputBuses[0].silenceFlags = 0;

    ProcessData data{};
    data.processMode = kRealtime;
    data.symbolicSampleSize = kSample32;
    data.numSamples = frames;
    data.numInputs = static_cast<int32>(p->inputBuses.size());
    data.numOutputs = static_cast<int32>(p->outputBuses.size());
    data.inputs = p->inputBuses.data();
    data.outputs = p->outputBuses.data();
    data.inputParameterChanges = &p->inputChanges;

    if (p->processor->process(data) != kResultTrue) return VST3_ERR_INTERNAL;
    return VST3_OK;
    VST3_GUARD_END(VST3_ERR_INTERNAL)
}

// ===========================================================================
// Parameters
// ===========================================================================

int32_t __cdecl vst3_plugin_param_count(Vst3PluginHandle plugin) {
    VST3_GUARD_BEGIN
    auto* p = asPlugin(plugin);
    if (!p) return VST3_ERR_INVALID_HANDLE;
    if (!p->controller) return 0;
    return static_cast<int32_t>(p->controller->getParameterCount());
    VST3_GUARD_END(VST3_ERR_INTERNAL)
}

Vst3Status __cdecl vst3_plugin_param_info(Vst3PluginHandle plugin, int32_t index, Vst3ParamInfo* outInfo) {
    VST3_GUARD_BEGIN
    auto* p = asPlugin(plugin);
    if (!p) return VST3_ERR_INVALID_HANDLE;
    if (!outInfo) return VST3_ERR_INVALID_ARG;
    if (!p->controller) return VST3_ERR_INVALID_ARG;
    if (index < 0 || index >= p->controller->getParameterCount()) return VST3_ERR_INVALID_ARG;

    ParameterInfo info{};
    if (p->controller->getParameterInfo(index, info) != kResultTrue) return VST3_ERR_INVALID_ARG;

    std::memset(outInfo, 0, sizeof(*outInfo));
    outInfo->id = info.id;
    copyU16(outInfo->title, 128, std::u16string(reinterpret_cast<const char16_t*>(info.title)));
    copyU16(outInfo->units, 128, std::u16string(reinterpret_cast<const char16_t*>(info.units)));
    outInfo->defaultNormalized = info.defaultNormalizedValue;
    outInfo->stepCount = info.stepCount;
    outInfo->flags = paramFlagsFrom(info.flags);
    return VST3_OK;
    VST3_GUARD_END(VST3_ERR_INTERNAL)
}

double __cdecl vst3_plugin_get_param(Vst3PluginHandle plugin, uint32_t paramId) {
    VST3_GUARD_BEGIN
    auto* p = asPlugin(plugin);
    if (!p || !p->controller) return std::nan("");
    return p->controller->getParamNormalized(paramId);
    VST3_GUARD_END(std::nan(""))
}

Vst3Status __cdecl vst3_plugin_set_param(Vst3PluginHandle plugin, uint32_t paramId, double normalizedValue) {
    VST3_GUARD_BEGIN
    auto* p = asPlugin(plugin);
    if (!p) return VST3_ERR_INVALID_HANDLE;
    if (!(normalizedValue >= 0.0 && normalizedValue <= 1.0)) return VST3_ERR_INVALID_ARG;

    // §6.2: queue for the processor (picked up at the next block) ...
    p->paramQueue.push(paramId, normalizedValue);
    // ... and push to the controller now so the editor reflects it.
    if (p->controller) p->controller->setParamNormalized(paramId, normalizedValue);
    return VST3_OK;
    VST3_GUARD_END(VST3_ERR_INTERNAL)
}

// ===========================================================================
// State
// ===========================================================================

Vst3Status __cdecl vst3_plugin_get_state(Vst3PluginHandle plugin, uint8_t** outBuf, int32_t* outLen) {
    VST3_GUARD_BEGIN
    auto* p = asPlugin(plugin);
    if (!p) return VST3_ERR_INVALID_HANDLE;
    if (!outBuf || !outLen) return VST3_ERR_INVALID_ARG;
    *outBuf = nullptr;
    *outLen = 0;

    MemoryStream stream;
    if (p->component->getState(&stream) != kResultTrue) return VST3_ERR_STATE;

    const int64 size = stream.getSize();
    if (size < 0 || size > INT32_MAX) return VST3_ERR_STATE;

    auto* buf = static_cast<uint8_t*>(std::malloc(static_cast<size_t>(size) + 1));
    if (!buf) return VST3_ERR_INTERNAL;
    if (size > 0) std::memcpy(buf, stream.getData(), static_cast<size_t>(size));

    *outBuf = buf;
    *outLen = static_cast<int32_t>(size);
    return VST3_OK;
    VST3_GUARD_END(VST3_ERR_INTERNAL)
}

Vst3Status __cdecl vst3_plugin_set_state(Vst3PluginHandle plugin, const uint8_t* buf, int32_t len) {
    VST3_GUARD_BEGIN
    auto* p = asPlugin(plugin);
    if (!p) return VST3_ERR_INVALID_HANDLE;
    if (!buf || len < 0) return VST3_ERR_INVALID_ARG;

    MemoryStream stream(reinterpret_cast<void*>(const_cast<uint8_t*>(buf)), len);
    stream.seek(0, IBStream::kIBSeekSet, nullptr);
    if (p->component->setState(&stream) != kResultTrue) return VST3_ERR_STATE;

    // Mirror the restored state into the controller so the editor and get_param agree.
    if (p->controller) {
        stream.seek(0, IBStream::kIBSeekSet, nullptr);
        p->controller->setComponentState(&stream);
    }
    return VST3_OK;
    VST3_GUARD_END(VST3_ERR_INTERNAL)
}

void __cdecl vst3_free_buffer(uint8_t* buf) {
    VST3_GUARD_BEGIN
    if (buf) std::free(buf);
    VST3_GUARD_END_VOID
}

// ===========================================================================
// Editor (UI thread only)
// ===========================================================================

int32_t __cdecl vst3_plugin_has_editor(Vst3PluginHandle plugin) {
    VST3_GUARD_BEGIN
    auto* p = asPlugin(plugin);
    if (!p) return VST3_ERR_INVALID_HANDLE;
    if (!p->controller) return 0;
    if (p->view) return 1;

    IPtr<IPlugView> probe = owned(p->controller->createView(ViewType::kEditor));
    if (!probe) return 0;
    return probe->isPlatformTypeSupported(kPlatformTypeHWND) == kResultTrue ? 1 : 0;
    VST3_GUARD_END(VST3_ERR_INTERNAL)
}

Vst3Status __cdecl vst3_plugin_open_editor(Vst3PluginHandle plugin, void* parentHwnd) {
    VST3_GUARD_BEGIN
    auto* p = asPlugin(plugin);
    if (!p) return VST3_ERR_INVALID_HANDLE;
    if (!parentHwnd) return VST3_ERR_INVALID_ARG;
    if (!p->controller) return VST3_ERR_NO_EDITOR;
    if (p->view) p->closeEditor();  // re-parenting: drop the old view first

    p->view = owned(p->controller->createView(ViewType::kEditor));
    if (!p->view) return VST3_ERR_NO_EDITOR;
    if (p->view->isPlatformTypeSupported(kPlatformTypeHWND) != kResultTrue) {
        p->view = nullptr;
        return VST3_ERR_NO_EDITOR;
    }

    // Before attached(), never after: plug-ins call back into the frame during attach,
    // and a null frame is a crash rather than a missing feature.
    p->plugFrame.setParent(parentHwnd);
    p->view->setFrame(&p->plugFrame);

    if (p->view->attached(parentHwnd, kPlatformTypeHWND) != kResultTrue) {
        p->view->setFrame(nullptr);
        p->view = nullptr;
        return VST3_ERR_INTERNAL;
    }
    return VST3_OK;
    VST3_GUARD_END(VST3_ERR_INTERNAL)
}

Vst3Status __cdecl vst3_plugin_get_editor_size(Vst3PluginHandle plugin, int32_t* outWidth, int32_t* outHeight) {
    VST3_GUARD_BEGIN
    auto* p = asPlugin(plugin);
    if (!p) return VST3_ERR_INVALID_HANDLE;
    if (!outWidth || !outHeight) return VST3_ERR_INVALID_ARG;
    *outWidth = 0;
    *outHeight = 0;
    if (!p->controller) return VST3_ERR_NO_EDITOR;

    // Queryable before open (§3): use the live view if attached, else a throwaway probe.
    IPtr<IPlugView> view = p->view;
    if (!view) {
        view = owned(p->controller->createView(ViewType::kEditor));
        if (!view) return VST3_ERR_NO_EDITOR;
    }

    ViewRect rect{};
    if (view->getSize(&rect) != kResultTrue) return VST3_ERR_NO_EDITOR;
    *outWidth = rect.getWidth();
    *outHeight = rect.getHeight();
    return VST3_OK;
    VST3_GUARD_END(VST3_ERR_INTERNAL)
}

Vst3Status __cdecl vst3_plugin_close_editor(Vst3PluginHandle plugin) {
    VST3_GUARD_BEGIN
    auto* p = asPlugin(plugin);
    if (!p) return VST3_ERR_INVALID_HANDLE;
    return p->closeEditor();  // safe when nothing is open (§3)
    VST3_GUARD_END(VST3_ERR_INTERNAL)
}
