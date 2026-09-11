# NetPlugHost → StreamRecorder handoff

Read this together with [`ABI_SPEC.md`](ABI_SPEC.md) §7, which defines the adapter contract.
This file records what is **built and proven**, what is **built but unproven**, and the
non-obvious things that will cost you a debugging session if you don't know them up front.

NetPlugHost is complete and green. Nothing in this repo needs changing to start — if you find
yourself wanting to change the ABI, stop and update `ABI_SPEC.md` first in both repos (§13).

---

## 1. Referencing it

```powershell
cd d:\git\NetPlugHost
git submodule update --init --recursive
.\build.ps1
```

Outputs:

| File | Path |
|---|---|
| `NetPlugHost.dll` | `managed\NetPlugHost\bin\x64\Release\net9.0-windows\win-x64\` |
| `Vst3HostNative.dll` | `native\build\bin\` (also copied next to the managed DLL) |

Add a `ProjectReference` to `managed\NetPlugHost\NetPlugHost.csproj`.

### Two build requirements that are not optional

**StreamRecorder must be x64.** The native DLL has no x86 build (§2). Set
`<PlatformTarget>x64</PlatformTarget>`.

**You must import the native targets file**, or you will get a `DllNotFoundException` at the
first call:

```xml
<Import Project="..\..\NetPlugHost\NetPlugHost.Native.targets" />
```

A `ProjectReference` does **not** carry loose native files into the referencing project's output
directory. `NetPlugHost.dll` will be copied; `Vst3HostNative.dll` will not. The import adds a copy
target that puts it beside your binary.

> Do **not** declare `<Platforms>x64</Platforms>`. It leaves the solution's `Any CPU`
> configuration with nothing to map to, and MSBuild then **skips the project while still printing
> "Build succeeded"** — you get a silently stale binary. `PlatformTarget` alone is correct. This
> cost real debugging time here; don't repeat it.

---

## 2. API surface

```csharp
// Loading
Vst3Module module = Vst3Module.Load(absolutePath);   // throws Vst3Exception(LoadFailed)
int         count  = module.ClassCount;              // audio-effect classes only
Vst3ClassInfo ci   = module.GetClassInfo(0);         // Name, Category, Vendor, Version, IsAudioEffect
IReadOnlyList<Vst3ClassInfo> all = module.GetClasses();
Vst3Plugin  plugin = module.CreatePlugin(classIndex: 0);

// Processing lifecycle  (Setup -> SetActive(true) -> Process... -> SetActive(false))
plugin.Setup(sampleRate, maxBlockSize, inputChannels, outputChannels);  // 1 or 2 channels
plugin.SetActive(true);
int latency = plugin.LatencySamples;    // per channel; read AFTER SetActive(true)
plugin.Reset();                         // clears tails; requires a completed Setup
int max = plugin.MaxBlockSize;          // plus InputChannels / OutputChannels

// Processing — planar, in-place allowed
unsafe Vst3Status Process(float** inputs, float** outputs, int frames);       // hot path, no throw
void Process(ReadOnlySpan<IntPtr> inputs, ReadOnlySpan<IntPtr> outputs, int frames); // throws

// Parameters — always normalized [0,1]
int   n    = plugin.ParameterCount;
var   info = plugin.GetParameterInfo(i);   // Id, Title, Units, DefaultNormalized, StepCount, Flags
var   ps   = plugin.GetParameters();
double v   = plugin.GetParameter(paramId);
plugin.SetParameter(paramId, normalized);  // safe to call while Process runs

// State
byte[] blob = plugin.GetState();           // already copied to managed memory; nothing to free
plugin.SetState(blob);

// Editor — UI thread only
bool has = plugin.HasEditor;
var (w, h) = plugin.GetEditorSize();       // valid before opening, to size your window
plugin.OpenEditor(hwnd);
plugin.CloseEditor();

plugin.Dispose();   // dispose ALL plugins before the module
module.Dispose();   // throws InvalidOperationException if any plugin is still alive
```

Errors surface as `Vst3Exception` carrying a `Vst3Status`. Expected outcomes are modelled as
values instead — `HasEditor` returns `false` rather than throwing.

---

## 3. What to build

Per §1 and §7, three pieces, all in StreamRecorder:

### 3a. `Audio/Effects/Vst3Effect : IAudioEffect`

The mapping table is §7. The parts that carry real risk:

- **Interleave ↔ planar is yours.** The ABI is planar (VST3's native layout); `IAudioEffect.Process`
  is interleaved. Allocate reusable planar scratch buffers in `Prepare` — never in `Process`.
- **Chunking is yours.** `Process` may receive any length; split into `<= MaxBlockSize` pieces
  (§6.5). Short final blocks are fine and are tested.
- **Use the `float**` overload on the audio thread.** It returns a status and does not allocate or
  throw. The span overload validates and throws — fine for setup paths, not for the render loop.
- **`Enabled == false` short-circuits before any native call** (§7). A disabled VST costs nothing.
- **`Prepare` may be called again** on a format change. `Setup` handles re-preparation internally
  (it deactivates and reactivates), but re-allocate your scratch buffers to the new size.

### 3b. WPF `HwndHost` editor window

Size the host window from `GetEditorSize()` *before* `OpenEditor(hwnd)`. All editor calls must be
on the UI thread. `CloseEditor` before disposing the plugin (`Dispose` also does it defensively).

### 3c. "Load VST3…" UI + parameter list

`Vst3Module.Load` → `GetClasses()` → `CreatePlugin(index)`. Parameter values crossing the boundary
are **always normalized [0,1]** — denormalized display values (dB, Hz) stay inside the plugin.
If you want to show "-6.0 dB" you will need a `getParamStringByValue` equivalent, which the v1 ABI
does **not** expose. Flag it rather than inventing one; it's an ABI change.

---

## 4. Latency — read this before writing the offline render

`Process` returns each block delayed by the plugin's latency. **The host does not compensate; you
do** (§6.3). `LatencySamples` is **per channel**, matching `IAudioEffect.LatencySamples`.

- **Live capture insert:** no compensation. Accept the inherent delay.
- **Offline commit/export:** feed `latency` extra frames of silence *after* the real signal, then
  drop the first `latency` output samples. Otherwise the result is time-shifted and the tail is
  clipped.

This is the single easiest thing to get subtly wrong — it produces output that sounds fine in
isolation but drifts against the untreated signal. Test it explicitly (see §6).

---

## 5. Threading

| Thread | Calls |
|---|---|
| Audio/render | `Process` **only** |
| UI/control | everything else — editor calls especially |
| Either | `SetParameter` — the one deliberately cross-thread call |

`SetParameter` is safe to call from the UI while `Process` runs (a lock-free queue drained at the
top of each block). **No other call is safe concurrently with `Process` on the same handle** —
including `SetActive`, `Setup`, `GetState`, and `Reset`. If your UI can trigger those while the
capture chain is live, you need your own gate.

Never call `Process` on two threads for the same plugin at once. The live insert and the offline
render must not share a `Vst3Plugin` instance concurrently.

---

## 6. What still needs proving — this is where your testing effort should go

NetPlugHost's own tests cover the ABI headlessly. These are the things only StreamRecorder can
exercise, roughly in order of risk:

1. **The editor path is genuinely unproven.** `OpenEditor` needs a real HWND on a UI thread and has
   never been executed — only `HasEditor` and `GetEditorSize` are covered. Expect to find problems
   here first. Test: open, close, reopen, resize the host window, close the window with the editor
   still open, and dispose the plugin without calling `CloseEditor`.
2. **Latency compensation on offline render.** Use a plugin with non-zero latency (NI `Vari Comp`
   reports 4 samples; a linear-phase EQ or lookahead limiter gives you a large one). Render a known
   transient and assert it lands at the same sample index as the dry signal.
3. **Interleave/planar round-trip.** Process with a plugin set to unity/bypass and assert the output
   is sample-identical to the input. This catches channel swaps and off-by-one interleaving
   immediately, which a gain test will not.
4. **Chunking.** Drive buffer lengths that are not multiples of `MaxBlockSize` — especially a final
   short block — and confirm the output matches a single large-block render.
5. **Format changes.** Call `Prepare` again with a different sample rate and channel count on a live
   effect; confirm no crash and no stale scratch buffers.
6. **`UnsupportedIo` handling.** §6.4 guarantees you either get exactly the channel count you asked
   for or a hard failure — never a silent substitution. Confirm your adapter reports the failure and
   passes the signal through dry rather than doing wrong interleave math.
7. **Session persistence.** `GetState` → save → reload → `SetState`, and confirm the editor reflects
   the restored values.
8. **Concurrent `SetParameter`.** Automate a parameter from the UI while the capture chain runs;
   listen for zipper noise or crashes.
9. **Plugin variety.** The native smoke test passes against nine real plugins, but only through the
   C ABI. Exercise a synth-adjacent effect, something with a sidechain, and something with a large
   editor through the full app path.

---

## 7. Known-good reference behaviour

From this repo's tests, so you have expected values:

- SDK fixture `again-sample-accurate`, gain at 0.25 normalized: input RMS `0.351286` →
  output `0.087822` (exactly 0.25×), stable across consecutive blocks.
- State blob for that fixture: 28 bytes.
- 20 load/create/process/destroy cycles leak-free.
- Real plugins verified through the C ABI: Vari Comp, bx_solo, Solid EQ, Raum, Replika,
  Supercharger, Flair, Transient Master, VC 76.

To sanity-check the stack independently of StreamRecorder at any point:

```powershell
d:\git\NetPlugHost\native\build\bin\Vst3HostSmoke.exe "C:\Program Files\Common Files\VST3\Raum.vst3"
dotnet run --project d:\git\NetPlugHost\tests\NetPlugHost.HeadlessTest -- "<path.vst3>"
```

If something misbehaves in-app, run the smoke test against the same plugin first. It takes the CLR
and StreamRecorder out of the picture and tells you within seconds which side the fault is on.

---

## 8. Licensing

`Vst3HostNative.dll` links the Steinberg VST3 SDK under the **GPLv3** option. Anything that links
it — including StreamRecorder — inherits GPLv3 obligations **when distributed**. Fine for local
development. If StreamRecorder is ever to ship proprietary, that requires registering for
Steinberg's (free) proprietary licence and updating `native/README.md`. Raise it before any
public release rather than after.
