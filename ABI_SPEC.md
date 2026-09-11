# NetPlugHost — ABI Specification (v1)

This is the **pinned contract** between two independently built codebases:

- **NetPlugHost** (this repo, `d:\git\NetPlugHost`): a native VST3 host + a managed C# wrapper.
- **StreamRecorder** (`d:\git\StreamRecorder`): a WPF audio app that consumes NetPlugHost through a
  small adapter (`Vst3Effect : IAudioEffect`) plus an editor window.

Build both sides to this document. The only two things that must agree exactly are (a) the flat
`extern "C"` ABI in [§3](#3-c-abi), and (b) how it maps onto StreamRecorder's `IAudioEffect`
([§7](#7-mapping-onto-iaudioeffect)). Everything else on either side is free to change.

**If you need to change the ABI, change it here first**, in both this file and the consuming adapter,
and bump the version. Runtime ABI drift = crashes, not compile errors.

---

## 1. Deliverables

NetPlugHost produces three things, in dependency order:

1. **`NetPlugHost.Native` → `Vst3HostNative.dll`** — native C++ (CMake + MSVC), **x64 only**. Links the
   Steinberg VST3 SDK. Loads `.vst3` bundles, runs `setupProcessing`/`process`, parameters, state,
   and the `IPlugView` editor. Exposes exactly the flat C API in §3. No C++ types cross the boundary.
2. **`NetPlugHost` (managed)** — a C# class library (`net9.0-windows`, **x64**) that P/Invokes
   `Vst3HostNative.dll` and presents clean, disposable types (`Vst3Module`, `Vst3Plugin`, info
   records). This is the public package StreamRecorder references. Generic, reusable, testable — it
   must **not** depend on StreamRecorder or on `IAudioEffect`.
3. A **headless test** (console or xUnit) that loads a known free VST3, sets up processing, pushes a
   buffer through `process`, and asserts the output changed (see §8).

StreamRecorder builds component **3 of Phase 3** itself: the `Vst3Effect` adapter, the WPF `HwndHost`
editor window, and the "Load VST3…" UI. That code is out of scope for this repo — but §7 is written so
the adapter and the managed wrapper meet cleanly.

### Packaging

Ship the managed assembly with `Vst3HostNative.dll` (and any VST3 SDK runtime deps) laid down next to
it so a consuming x64 app finds the native DLL by default probing. A NuGet package with a
`runtimes/win-x64/native/` folder is ideal; a plain "copy these files" output is acceptable for v1.

---

## 2. Conventions (apply to every function in §3)

- **Calling convention:** `__cdecl` (the default for `extern "C"` on x64 Windows — but state it
  explicitly with `__cdecl` so the managed `[DllImport]`/`LibraryImport` uses `CallingConvention.Cdecl`).
- **Export:** every function is `extern "C"` with `__declspec(dllexport)` (or a `.def` file). Names are
  exactly as written — no C++ mangling, no leading underscore, no stdcall `@N` suffix.
- **Handles are opaque.** `void*`. Never dereferenced by the caller. `NULL` is never a valid handle.
- **Return type `Vst3Status`** (`int32`): `0` = OK; **negative** = error (codes in §4). Functions that
  return a value instead (counts, sizes, `double` param values) say so and use out-of-range sentinels
  as noted per function.
- **Strings crossing the boundary are UTF-16** (`char16_t`, i.e. `wchar_t` on Windows — 2 bytes),
  **null-terminated**, in **fixed-size struct arrays** (§5). No heap strings, no free dance. Paths
  passed *in* are `const wchar_t*`, null-terminated.
- **The DLL is x64.** There is no x86 build. This forces StreamRecorder to an x64 build too (expected).
- **No exceptions cross the boundary.** The native side catches everything at the C seam and converts
  to a `Vst3Status`. A C++ exception reaching the ABI edge is a bug.

---

## 3. C ABI

Header sketch (`vst3_host_c.h`). This is the source of truth for signatures; the managed wrapper
mirrors it 1:1.

```c
#include <uchar.h>   // char16_t

#ifdef __cplusplus
extern "C" {
#endif

typedef void* Vst3ModuleHandle;   // a loaded .vst3 bundle
typedef void* Vst3PluginHandle;   // one instantiated plug-in (component + controller)
typedef int32_t Vst3Status;       // 0 = OK, negative = error (see §4)

/* ---- structs: see §5 for field docs ---- */
typedef struct Vst3ClassInfo Vst3ClassInfo;
typedef struct Vst3ParamInfo Vst3ParamInfo;

/* =========================================================================
 * Module lifecycle
 * ========================================================================= */

/* Load a .vst3 bundle from an absolute path. On success *outModule is a non-NULL handle.
 * The module stays loaded until vst3_module_free. */
Vst3Status __cdecl vst3_module_load(const wchar_t* path, Vst3ModuleHandle* outModule);

/* Number of audio-effect classes the bundle's factory exposes (>= 0). Negative on bad handle. */
int32_t __cdecl vst3_module_class_count(Vst3ModuleHandle module);

/* Fill *outInfo for class [index] in [0, class_count). */
Vst3Status __cdecl vst3_module_class_info(Vst3ModuleHandle module, int32_t index, Vst3ClassInfo* outInfo);

/* Unload the bundle. All plugins created from it MUST already be destroyed. No-op on NULL. */
void __cdecl vst3_module_free(Vst3ModuleHandle module);

/* =========================================================================
 * Plugin lifecycle
 * ========================================================================= */

/* Instantiate class [classIndex] from the module: creates the processor (IComponent /
 * IAudioProcessor) and its edit controller, connects them. *outPlugin non-NULL on success. */
Vst3Status __cdecl vst3_plugin_create(Vst3ModuleHandle module, int32_t classIndex, Vst3PluginHandle* outPlugin);

/* Tear down one plugin (closes the editor first if open). No-op on NULL. */
void __cdecl vst3_plugin_destroy(Vst3PluginHandle plugin);

/* =========================================================================
 * Processing setup  (call order: setup -> set_active(1) -> process* -> set_active(0))
 * ========================================================================= */

/* Negotiate a stereo/mono bus arrangement matching inputChannels/outputChannels, set up processing
 * with 32-bit float, the given sample rate, and the given MAX block size (frames). Safe to call
 * again to re-prepare (the host will deactivate/reactivate internally). See §6 for channel rules. */
Vst3Status __cdecl vst3_plugin_setup(Vst3PluginHandle plugin, double sampleRate, int32_t maxBlockSize,
                                     int32_t inputChannels, int32_t outputChannels);

/* setActive + setProcessing. active != 0 turns the plugin on (must follow a successful setup);
 * active == 0 turns it off (flushes). */
Vst3Status __cdecl vst3_plugin_set_active(Vst3PluginHandle plugin, int32_t active);

/* Reported processing latency in samples PER CHANNEL (IAudioProcessor::getLatencySamples).
 * >= 0; negative on bad handle. May change after setup; read it after set_active(1). */
int32_t __cdecl vst3_plugin_latency_samples(Vst3PluginHandle plugin);

/* Clear internal state (tails, filter memory) without re-allocating — a fresh stream follows.
 * Implemented as setProcessing(false)+setProcessing(true) or an equivalent flush. */
Vst3Status __cdecl vst3_plugin_reset(Vst3PluginHandle plugin);

/* =========================================================================
 * Processing  (audio/render thread — see §6 threading)
 * ========================================================================= */

/* Process ONE block of up to maxBlockSize frames, DEINTERLEAVED (planar).
 *   inputs[c]  points to `frames` floats for input channel c   (inputChannels arrays)
 *   outputs[c] points to `frames` floats for output channel c  (outputChannels arrays)
 * In-place is allowed: the caller may pass the same pointers for inputs and outputs.
 * The host applies any queued parameter changes (from vst3_plugin_set_param) at the start of the
 * block. `frames` must be <= the maxBlockSize from setup. Does NOT latency-compensate (see §6.3). */
Vst3Status __cdecl vst3_plugin_process(Vst3PluginHandle plugin,
                                       const float* const* inputs,
                                       float* const* outputs,
                                       int32_t frames);

/* =========================================================================
 * Parameters
 * ========================================================================= */

int32_t   __cdecl vst3_plugin_param_count(Vst3PluginHandle plugin);              /* >=0, neg on bad handle */
Vst3Status __cdecl vst3_plugin_param_info(Vst3PluginHandle plugin, int32_t index, Vst3ParamInfo* outInfo);

/* Current normalized value [0,1] of the parameter with id `paramId`. NaN on bad handle/id. */
double    __cdecl vst3_plugin_get_param(Vst3PluginHandle plugin, uint32_t paramId);

/* Queue a normalized [0,1] change for `paramId`. Thread-safe vs. process (see §6.2): the change is
 * applied to the controller immediately and to the processor at the next process block. */
Vst3Status __cdecl vst3_plugin_set_param(Vst3PluginHandle plugin, uint32_t paramId, double normalizedValue);

/* =========================================================================
 * State (opaque plugin preset blob — IComponent::get/setState + controller state)
 * ========================================================================= */

/* Allocate and return the plugin's full state. *outBuf/*outLen owned by the CALLEE until the caller
 * releases it with vst3_free_buffer. On success *outBuf != NULL, *outLen >= 0. */
Vst3Status __cdecl vst3_plugin_get_state(Vst3PluginHandle plugin, uint8_t** outBuf, int32_t* outLen);

/* Restore a state blob previously returned by vst3_plugin_get_state. */
Vst3Status __cdecl vst3_plugin_set_state(Vst3PluginHandle plugin, const uint8_t* buf, int32_t len);

/* Free a buffer returned by vst3_plugin_get_state. No-op on NULL. */
void __cdecl vst3_free_buffer(uint8_t* buf);

/* =========================================================================
 * Editor (IPlugView) — UI THREAD ONLY (see §6.1)
 * ========================================================================= */

int32_t   __cdecl vst3_plugin_has_editor(Vst3PluginHandle plugin);   /* 1 yes, 0 no, neg on bad handle */

/* Create the plugin's IPlugView and attach it to parentHwnd (an HWND cast to void*). The parent
 * window's client area should be sized to the editor size (query it first).
 *
 * The host installs its own IPlugFrame via IPlugView::setFrame() before calling attached(), and
 * clears it on close. This is REQUIRED, not optional: plug-ins may call plugFrame->resizeView()
 * from inside attached() when their editor sizes or scales itself, and one that does not
 * null-check its frame will dereference null and take the host process down. Leaving the frame
 * null crashed every Native Instruments plug-in tested (Battery 4, Kontakt 8, Guitar Rig 7, and
 * Super 8 with a bare 0xC0000005), all of which load correctly in any DAW.
 *
 * resizeView() resizes parentHwnd and forwards the new size to IPlugView::onSize(), so a plug-in
 * that asks to resize gets a host window that follows it. */
Vst3Status __cdecl vst3_plugin_open_editor(Vst3PluginHandle plugin, void* parentHwnd);

/* Current editor size in pixels. Valid after has_editor==1; may be queried before open to size the
 * host window. */
Vst3Status __cdecl vst3_plugin_get_editor_size(Vst3PluginHandle plugin, int32_t* outWidth, int32_t* outHeight);

/* Detach and release the IPlugView. Safe to call if no editor is open. Also called by destroy. */
Vst3Status __cdecl vst3_plugin_close_editor(Vst3PluginHandle plugin);

#ifdef __cplusplus
}
#endif
```

---

## 4. Status codes (`Vst3Status`)

`0` = OK. Negative = error. Keep these stable; add new codes with new (more negative) values.

| Value | Name                        | Meaning                                                        |
|------:|-----------------------------|----------------------------------------------------------------|
|   `0` | `VST3_OK`                   | Success.                                                       |
|  `-1` | `VST3_ERR_INVALID_HANDLE`   | Handle was NULL or not one this DLL issued.                    |
|  `-2` | `VST3_ERR_INVALID_ARG`      | Null out-pointer, index out of range, bad param id, etc.       |
|  `-3` | `VST3_ERR_LOAD_FAILED`      | Bundle not found / not a valid VST3 / factory missing.         |
|  `-4` | `VST3_ERR_NO_AUDIO_CLASS`   | Bundle has no audio-effect class (for create with a bad index).|
|  `-5` | `VST3_ERR_UNSUPPORTED_IO`   | Requested channel arrangement rejected by the plugin (§6).     |
|  `-6` | `VST3_ERR_NOT_SETUP`        | process/set_active called before a successful setup.           |
|  `-7` | `VST3_ERR_STATE`            | get/setState failed or blob rejected.                          |
|  `-8` | `VST3_ERR_NO_EDITOR`        | open/size editor when the plugin has no view.                  |
|  `-9` | `VST3_ERR_INTERNAL`         | Any other native failure (a caught C++ exception maps here).   |

The managed wrapper converts negatives to a `Vst3Exception` (carrying the code) — except where a "not
found / unsupported" outcome is expected and better modeled as a bool/empty result (e.g. `has_editor`).

---

## 5. Structs

Fixed layout, no padding surprises — use `int32_t`/`uint32_t`/`double` and fixed `char16_t[128]`
arrays so the managed side can blit them. All string fields are UTF-16, null-terminated, truncated to
fit.

```c
struct Vst3ClassInfo {
    char16_t name[128];       /* class (plugin) name */
    char16_t category[128];   /* VST3 subcategories, e.g. "Fx|EQ" */
    char16_t vendor[128];
    char16_t version[64];
    int32_t  isAudioEffect;   /* 1 if this class is an audio-effect component; 0 otherwise */
};

struct Vst3ParamInfo {
    uint32_t id;                 /* ParamID — pass to get/set_param */
    char16_t title[128];
    char16_t units[128];
    double   defaultNormalized;  /* [0,1] */
    int32_t  stepCount;          /* 0 = continuous; >0 = discrete steps */
    int32_t  flags;              /* bit0 canAutomate, bit1 isBypass, bit2 isReadOnly, bit3 isList */
};
```

The managed wrapper exposes these as records (`Vst3ClassInfo`, `Vst3ParamInfo`) with `string`
properties and a `[Flags] enum Vst3ParamFlags`.

---

## 6. Semantics that must match

### 6.1 Threading

Two thread contexts, and the boundary between them is strict:

- **Audio/render thread:** `vst3_plugin_process` only. Must be allocation-free and lock-free on the
  hot path (bounded work). StreamRecorder calls it from a capture callback (live insert) and from an
  offline render worker (commit/export) — never concurrently on the same handle.
- **UI/main thread:** everything else, in particular **all editor calls**
  (`has_editor`/`open_editor`/`get_editor_size`/`close_editor`) — VST3 `IPlugView` requires the main
  thread. `vst3_module_*`, `vst3_plugin_create`/`destroy`, `setup`, `set_active`, state, and param
  enumeration are also expected on the UI/control thread.

`vst3_plugin_set_param` is the **one deliberately cross-thread** call: it may be called from the UI
thread while `process` runs on the audio thread. The native host must make this safe (a lock-free
param queue drained at the top of each `process` block; also push to the edit controller so the editor
reflects it). No other function is safe to call concurrently with `process` on the same handle.

### 6.2 Parameter model

Values crossing the ABI are always **normalized `[0,1]`** (VST3's native convention) — never plain
values. Denormalization (dB, Hz, …) stays inside the plugin/host. `set_param` queues a change applied
at the next block; `get_param` returns the controller's current normalized value.

### 6.3 Latency — the host does NOT compensate; the caller does

`vst3_plugin_process` returns each block delayed by the plugin's reported latency; it does **not**
shift or trim. `vst3_plugin_latency_samples` reports that delay (per channel). StreamRecorder's
offline commit/export render is responsible for compensation: feed `latency` extra frames of silence
after the real signal, then drop the first `latency` output samples so the result is time-aligned and
the tail isn't clipped. This is exactly why `IAudioEffect.LatencySamples` exists on the app side —
keep the two definitions identical (**samples per channel**).

### 6.4 Channels / bus arrangement

`setup` requests `inputChannels`/`outputChannels` (StreamRecorder only ever uses 1 or 2, and always
in == out). The host maps 1→mono bus, 2→stereo bus and calls `setBusArrangements`. If the plugin
refuses the requested arrangement, return `VST3_ERR_UNSUPPORTED_IO` (do **not** silently substitute a
different channel count — the caller's interleave math depends on getting what it asked for). The
adapter then reports the failure and passes the signal through dry. (A future nicety — auto-wrapping a
mono feed into a stereo-only plugin — is explicitly out of scope for v1.)

### 6.5 Block size

`setup`'s `maxBlockSize` is the ceiling; `process` may be called with any `frames <= maxBlockSize`
(including a short final block). The adapter chunks arbitrary-length buffers into `<= maxBlockSize`
pieces. The host must not assume a constant block size.

---

## 7. Mapping onto `IAudioEffect`

This is the StreamRecorder-side contract the adapter (`Audio/Effects/Vst3Effect : IAudioEffect`) will
implement in *that* repo. Reproduced here so NetPlugHost can see the target it's being consumed
against. **The managed wrapper does not implement `IAudioEffect`** — it stays generic; the adapter is
the glue.

```csharp
public interface IAudioEffect
{
    string Name { get; }
    bool   Enabled { get; set; }     // when false, adapter skips native process — signal passes dry
    int    LatencySamples { get; }   // per channel; caller compensates on offline render
    void   Prepare(int sampleRate, int channels);   // called before first block & on format change
    void   Process(float[] buffer, int offset, int count); // INTERLEAVED, in place; count = frames*channels
    void   Reset();                                  // clear state, no alloc
}
```

Adapter ⇄ NetPlugHost mapping:

| `IAudioEffect` member          | NetPlugHost call(s)                                                        |
|--------------------------------|---------------------------------------------------------------------------|
| `Name`                         | `Vst3ClassInfo.name` captured at load                                     |
| `Enabled == false`             | adapter returns immediately (no native call — dry passthrough)            |
| `LatencySamples`               | `vst3_plugin_latency_samples`                                             |
| `Prepare(sampleRate, channels)`| `vst3_plugin_setup(sr, maxBlock, channels, channels)` then `set_active(1)`; adapter (re)allocates the planar scratch + deinterleave buffers here |
| `Process(buf, off, count)`     | deinterleave `count/channels` frames → planar; chunk ≤ `maxBlock`; `vst3_plugin_process` per chunk; re-interleave back into `buf` |
| `Reset()`                      | `vst3_plugin_reset`                                                       |
| editor window (`HwndHost`)     | `has_editor`/`get_editor_size`/`open_editor(hwnd)`/`close_editor`         |
| "Load VST3…" + param UI        | `vst3_module_load`/`class_info`/`plugin_create`; `param_*`, `get/set_param` |
| session persistence (optional) | `get_state`/`set_state` + `vst3_free_buffer`                              |

**Key adapter obligations** (documented here so the ABI is sized for them):

- **Interleave↔planar** is entirely the adapter's job. The ABI is planar because that is VST3's native
  layout; the app's `IAudioEffect.Process` is interleaved because that is StreamRecorder's working
  format. The adapter owns reusable planar scratch buffers sized at `Prepare`.
- **Chunking** to `maxBlockSize` is the adapter's job (§6.5).
- **Latency compensation** is the *offline render's* job, not the live insert's (§6.3). The live
  capture insert (Phase 2) just processes in place and accepts the plugin's inherent latency in the
  monitored/recorded signal; the commit/export path aligns using `LatencySamples`.
- **`Enabled` is honored on the app side** — the adapter short-circuits before any native call, so a
  disabled VST costs nothing and needs no "bypass" round-trip.

Because it is just another `IAudioEffect`, the finished `Vst3Effect` drops into **both** the live
capture chain (Phase 2's `_liveChain`) and the Phase-1 selection commit/export path with no further
wiring.

---

## 8. Verification (NetPlugHost side)

Headless, no StreamRecorder involved:

1. `vst3_module_load` a known free VST3 (e.g. a simple gain/utility plugin), `class_count >= 1`,
   `class_info[0].isAudioEffect == 1`.
2. `vst3_plugin_create`, `vst3_plugin_setup(48000, 512, 2, 2)`, `set_active(1)`.
3. Enumerate params; `set_param` a gain-like parameter to a known non-unity value.
4. Fill two input channels with a known signal (e.g. a sine or a DC ramp), `vst3_plugin_process(512)`,
   assert the output differs from the input in the expected direction (level changed), and that a
   second identical block is consistent (state carried correctly).
5. `get_state` → non-empty blob; `set_state` it back with no error; `vst3_free_buffer`.
6. `set_active(0)`, `plugin_destroy`, `module_free` — no leak/crash under a repeat loop.

Editor (`open_editor`) is validated in-app on the StreamRecorder side (needs a real HWND on the UI
thread); a native smoke test that just checks `has_editor`/`get_editor_size` is enough here.

---

## 9. Out of scope for v1 (don't build unless asked)

- VST2 (a possible lower-effort fallback lives in StreamRecorder's follow-up list, not here).
- Multi-bus / side-chain / >2 channels, sample-accurate automation curves, MIDI/event I/O.
- Auto mono→stereo wrapping (§6.4).
- x86 / cross-platform (`.dll` is Windows x64 only; the VST3 SDK bundle loader is Windows-path based).

---

## 10. Licensing note (carry into the native build)

The Steinberg VST3 SDK is **dual GPLv3 / Steinberg proprietary**. A non-GPL distribution of
`Vst3HostNative.dll` requires accepting Steinberg's (free, registered) proprietary license. Keep the
SDK out of the committed tree (submodule or documented fetch) and record the license choice in the
native project's README.

---

*ABI v1 — authored from StreamRecorder's `IAudioEffect` seam and the Phase 2/3 plan. Any change to §3,
§4, §5, or §6 must be mirrored in the StreamRecorder `Vst3Effect` adapter and this version bumped.*
