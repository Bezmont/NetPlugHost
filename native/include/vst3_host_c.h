/* vst3_host_c.h — NetPlugHost flat C ABI, v1.
 *
 * Source of truth: ABI_SPEC.md §3 (signatures), §4 (status codes), §5 (structs).
 * The managed wrapper mirrors this 1:1. Any change here must be mirrored in
 * ABI_SPEC.md and in StreamRecorder's Vst3Effect adapter, and the version bumped.
 *
 * Conventions (§2): __cdecl, extern "C", opaque void* handles, UTF-16 strings in
 * fixed-size struct arrays, x64 only, no exceptions cross the boundary.
 */
#ifndef NETPLUGHOST_VST3_HOST_C_H
#define NETPLUGHOST_VST3_HOST_C_H

#include <stdint.h>
#include <uchar.h>   /* char16_t */
#include <wchar.h>

#ifdef VST3HOSTNATIVE_EXPORTS
#  define VST3_API __declspec(dllexport)
#else
#  define VST3_API __declspec(dllimport)
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef void*   Vst3ModuleHandle;   /* a loaded .vst3 bundle */
typedef void*   Vst3PluginHandle;   /* one instantiated plug-in (component + controller) */
typedef int32_t Vst3Status;         /* 0 = OK, negative = error */

/* ---- §4 status codes ---- */
#define VST3_OK                   0
#define VST3_ERR_INVALID_HANDLE  -1
#define VST3_ERR_INVALID_ARG     -2
#define VST3_ERR_LOAD_FAILED     -3
#define VST3_ERR_NO_AUDIO_CLASS  -4
#define VST3_ERR_UNSUPPORTED_IO  -5
#define VST3_ERR_NOT_SETUP       -6
#define VST3_ERR_STATE           -7
#define VST3_ERR_NO_EDITOR       -8
#define VST3_ERR_INTERNAL        -9

/* ---- §5 structs. Fixed layout, blittable. ---- */
typedef struct Vst3ClassInfo {
    char16_t name[128];       /* class (plugin) name */
    char16_t category[128];   /* VST3 subcategories, e.g. "Fx|EQ" */
    char16_t vendor[128];
    char16_t version[64];
    int32_t  isAudioEffect;   /* 1 if this class is an audio-effect component; 0 otherwise */
} Vst3ClassInfo;

typedef struct Vst3ParamInfo {
    uint32_t id;                 /* ParamID — pass to get/set_param */
    char16_t title[128];
    char16_t units[128];
    double   defaultNormalized;  /* [0,1] */
    int32_t  stepCount;          /* 0 = continuous; >0 = discrete steps */
    int32_t  flags;              /* bit0 canAutomate, bit1 isBypass, bit2 isReadOnly, bit3 isList */
} Vst3ParamInfo;

/* Vst3ParamInfo::flags bits */
#define VST3_PARAM_CAN_AUTOMATE  0x1
#define VST3_PARAM_IS_BYPASS     0x2
#define VST3_PARAM_IS_READ_ONLY  0x4
#define VST3_PARAM_IS_LIST       0x8

/* =========================================================================
 * Module lifecycle
 * ========================================================================= */

/* Load a .vst3 bundle from an absolute path. On success *outModule is non-NULL.
 * The module stays loaded until vst3_module_free. */
VST3_API Vst3Status __cdecl vst3_module_load(const wchar_t* path, Vst3ModuleHandle* outModule);

/* Number of audio-effect classes the bundle's factory exposes (>= 0). Negative on bad handle. */
VST3_API int32_t __cdecl vst3_module_class_count(Vst3ModuleHandle module);

/* Fill *outInfo for class [index] in [0, class_count). */
VST3_API Vst3Status __cdecl vst3_module_class_info(Vst3ModuleHandle module, int32_t index, Vst3ClassInfo* outInfo);

/* Unload the bundle. All plugins created from it MUST already be destroyed. No-op on NULL. */
VST3_API void __cdecl vst3_module_free(Vst3ModuleHandle module);

/* =========================================================================
 * Plugin lifecycle
 * ========================================================================= */

/* Instantiate class [classIndex]: creates the processor (IComponent / IAudioProcessor)
 * and its edit controller, connects them. *outPlugin non-NULL on success. */
VST3_API Vst3Status __cdecl vst3_plugin_create(Vst3ModuleHandle module, int32_t classIndex, Vst3PluginHandle* outPlugin);

/* Tear down one plugin (closes the editor first if open). No-op on NULL. */
VST3_API void __cdecl vst3_plugin_destroy(Vst3PluginHandle plugin);

/* =========================================================================
 * Processing setup  (call order: setup -> set_active(1) -> process* -> set_active(0))
 * ========================================================================= */

/* Negotiate a bus arrangement matching inputChannels/outputChannels, set up processing with
 * 32-bit float, the given sample rate and MAX block size (frames). Safe to call again to
 * re-prepare (deactivates/reactivates internally). See §6.4 for channel rules. */
VST3_API Vst3Status __cdecl vst3_plugin_setup(Vst3PluginHandle plugin, double sampleRate, int32_t maxBlockSize,
                                              int32_t inputChannels, int32_t outputChannels);

/* setActive + setProcessing. active != 0 turns the plugin on (must follow a successful setup);
 * active == 0 turns it off (flushes). */
VST3_API Vst3Status __cdecl vst3_plugin_set_active(Vst3PluginHandle plugin, int32_t active);

/* Reported processing latency in samples PER CHANNEL. >= 0; negative on bad handle.
 * May change after setup; read it after set_active(1). */
VST3_API int32_t __cdecl vst3_plugin_latency_samples(Vst3PluginHandle plugin);

/* Clear internal state (tails, filter memory) without re-allocating — a fresh stream follows. */
VST3_API Vst3Status __cdecl vst3_plugin_reset(Vst3PluginHandle plugin);

/* =========================================================================
 * Processing  (audio/render thread — §6.1)
 * ========================================================================= */

/* Process ONE block of up to maxBlockSize frames, DEINTERLEAVED (planar).
 *   inputs[c]  points to `frames` floats for input channel c   (inputChannels arrays)
 *   outputs[c] points to `frames` floats for output channel c  (outputChannels arrays)
 * In-place is allowed. Queued parameter changes are applied at the start of the block.
 * `frames` must be <= the maxBlockSize from setup. Does NOT latency-compensate (§6.3). */
VST3_API Vst3Status __cdecl vst3_plugin_process(Vst3PluginHandle plugin,
                                                const float* const* inputs,
                                                float* const* outputs,
                                                int32_t frames);

/* =========================================================================
 * Parameters
 * ========================================================================= */

VST3_API int32_t    __cdecl vst3_plugin_param_count(Vst3PluginHandle plugin);   /* >=0, neg on bad handle */
VST3_API Vst3Status __cdecl vst3_plugin_param_info(Vst3PluginHandle plugin, int32_t index, Vst3ParamInfo* outInfo);

/* Current normalized value [0,1] of `paramId`. NaN on bad handle/id. */
VST3_API double     __cdecl vst3_plugin_get_param(Vst3PluginHandle plugin, uint32_t paramId);

/* Queue a normalized [0,1] change. Thread-safe vs. process (§6.1): applied to the controller
 * immediately and to the processor at the next process block. */
VST3_API Vst3Status __cdecl vst3_plugin_set_param(Vst3PluginHandle plugin, uint32_t paramId, double normalizedValue);

/* =========================================================================
 * State (opaque plugin preset blob)
 * ========================================================================= */

/* Allocate and return the plugin's full state. *outBuf/*outLen owned by the CALLEE until the
 * caller releases it with vst3_free_buffer. On success *outBuf != NULL, *outLen >= 0. */
VST3_API Vst3Status __cdecl vst3_plugin_get_state(Vst3PluginHandle plugin, uint8_t** outBuf, int32_t* outLen);

/* Restore a state blob previously returned by vst3_plugin_get_state. */
VST3_API Vst3Status __cdecl vst3_plugin_set_state(Vst3PluginHandle plugin, const uint8_t* buf, int32_t len);

/* Free a buffer returned by vst3_plugin_get_state. No-op on NULL. */
VST3_API void __cdecl vst3_free_buffer(uint8_t* buf);

/* =========================================================================
 * Editor (IPlugView) — UI THREAD ONLY (§6.1)
 * ========================================================================= */

VST3_API int32_t __cdecl vst3_plugin_has_editor(Vst3PluginHandle plugin);   /* 1 yes, 0 no, neg on bad handle */

/* Create the plugin's IPlugView and attach it to parentHwnd (an HWND cast to void*). */
VST3_API Vst3Status __cdecl vst3_plugin_open_editor(Vst3PluginHandle plugin, void* parentHwnd);

/* Current editor size in pixels. Valid after has_editor==1; may be queried before open. */
VST3_API Vst3Status __cdecl vst3_plugin_get_editor_size(Vst3PluginHandle plugin, int32_t* outWidth, int32_t* outHeight);

/* Detach and release the IPlugView. Safe to call if no editor is open. Also called by destroy. */
VST3_API Vst3Status __cdecl vst3_plugin_close_editor(Vst3PluginHandle plugin);

#ifdef __cplusplus
}
#endif

#endif /* NETPLUGHOST_VST3_HOST_C_H */
