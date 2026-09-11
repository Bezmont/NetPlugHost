# NetPlugHost

A VST3 plug-in host for .NET on Windows. A native DLL wraps the Steinberg VST3 SDK behind a small,
flat C ABI, and a managed C# library puts safe, disposable types on top — so a .NET application can
load VST3 bundles, run audio through them, and show their editors without touching COM-style SDK
interfaces or C++.

**Windows x64 only. .NET 9.**

## What it can do

- **Load bundles and list their classes** — name, vendor, category and version for each audio
  processor class in a `.vst3` bundle (effects and instruments alike).
- **Instantiate plug-ins** — creates the processor and its edit controller and connects them.
- **Process audio** — 32-bit float, planar (deinterleaved), mono or stereo, in place if you like.
  The host negotiates the bus arrangement and never silently substitutes a different channel count.
- **Parameters** — enumerate them (title, units, default, step count, flags) and get or set
  normalized values, including from the UI thread while audio is running.
- **State** — save and restore a plug-in's complete state as an opaque blob.
- **Editors** — attach a plug-in's own GUI to any window handle (HWND) you supply, query its size
  first, and close it again. The host implements `IPlugFrame`, so editors that resize themselves
  work too.

Not included: MIDI or other event input (instruments load and show their editors, but cannot be
played), 64-bit float processing, and x86 or non-Windows builds.

## Used by

- **StreamRecorder** — applies VST3 effects in its audio chain through a small
  `Vst3Effect : IAudioEffect` adapter that lives in that repository.
- **[PlugBrowser](https://github.com/Bezmont/PlugBrowser)** — a browsable catalog of installed
  plug-ins, which loads each VST3 in a sandboxed process and photographs its editor.

## Repository layout

| Path | What it is |
|---|---|
| `native/include/vst3_host_c.h` | The flat C ABI. The source of truth; the managed side mirrors it one to one. |
| `native/src/vst3_host.cpp` | `Vst3HostNative.dll`: module and plug-in lifecycle, processing, parameters, state, editor hosting. |
| `native/test/smoke.cpp` | Native-only exercise of the ABI. No CLR involved, so a crash is attributable to the host. |
| `managed/NetPlugHost/` | `NetPlugHost.dll`: P/Invoke layer plus `Vst3Module` and `Vst3Plugin`. |
| `tests/NetPlugHost.HeadlessTest/` | End-to-end test of the whole ABI, plus contract checks. |
| `tests/fixture/` | Builds the SDK's `again-sample-accurate` gain plug-in as a deterministic test target. |
| `external/vst3sdk` | Steinberg VST3 SDK, as a git submodule. |
| `NetPlugHost.Native.targets` | MSBuild targets that copy the native DLL into a consuming project's output. |
| [`ABI_SPEC.md`](ABI_SPEC.md) | The pinned contract: every function, status code, struct and threading rule. |
| [`HANDOFF.md`](HANDOFF.md) | Integration notes — what is proven, and the traps worth knowing up front. |

## Building

Prerequisites: **Visual Studio 2022 with the "Desktop development with C++" workload** (MSVC x64,
Windows SDK and CMake) and the **.NET 9 SDK**.

```powershell
git clone --recursive https://github.com/Bezmont/NetPlugHost.git
cd NetPlugHost
.\build.ps1
```

Already cloned without `--recursive`? Fetch the SDK with `git submodule update --init --recursive`.

`build.ps1` builds the native DLL, the test fixture and the managed assembly in dependency order,
then runs both tests. Options: `-Configuration Debug` for SDK assertions, `-SkipFixture` and
`-SkipTest` to shorten the loop.

To build just the native DLL:

```powershell
cmake -S native -B native/build -A x64
cmake --build native/build --config Release
```

The output is `native/build/bin/Vst3HostNative.dll`. See [`native/README.md`](native/README.md) for
building against an SDK checkout elsewhere.

## Using it from .NET

Reference the managed project, target x64, and import the targets file so the native DLL is copied
next to your binaries — without it the first call fails with `DllNotFoundException`:

```xml
<PropertyGroup>
  <PlatformTarget>x64</PlatformTarget>
</PropertyGroup>

<ItemGroup>
  <ProjectReference Include="..\NetPlugHost\managed\NetPlugHost\NetPlugHost.csproj" />
</ItemGroup>

<Import Project="..\NetPlugHost\NetPlugHost.Native.targets" />
```

Then:

```csharp
using NetPlugHost;

using var module = Vst3Module.Load(@"C:\Program Files\Common Files\VST3\Some Plugin.vst3");

foreach (var info in module.GetClasses())
    Console.WriteLine($"{info.Name} by {info.Vendor} ({info.Category})");

using var plugin = module.CreatePlugin(classIndex: 0);
plugin.Setup(sampleRate: 48000, maxBlockSize: 512, inputChannels: 2, outputChannels: 2);
plugin.SetActive(true);

// On the audio thread: plugin.Process(inputs, outputs, frames) for each block.

plugin.SetActive(false);
```

Rules worth knowing before you start (all detailed in `ABI_SPEC.md`):

- **Dispose plug-ins before their module.** Disposing a module with live plug-ins throws rather than
  unloading code that is still in use.
- **Threading:** `Process` is the only audio-thread call. Everything else, and editor calls in
  particular, belongs on the UI thread. `SetParameter` is the one call that is also safe from the UI
  thread while audio runs.
- **Latency is reported, not compensated.** `LatencySamples` gives the plug-in's delay; aligning the
  output is the caller's job.
- **Keep blocks within `maxBlockSize`**; shorter blocks are fine.

## Verification

- The headless test passes end to end against the SDK fixture: bundle load, class enumeration,
  setup, a parameter round trip, in-place planar processing (input RMS 0.351 → 0.088, exactly the
  0.25 gain applied), a consistent second block, a state round trip, and 20
  load / create / process / destroy cycles with no crash.
- The native smoke test also passes against nine real third-party plug-ins (Native Instruments,
  Plugin Alliance, Arturia), including editor size queries and a side-chain compressor.
- Editor hosting is exercised at scale by PlugBrowser, which opens and captures the editors of
  nearly two hundred commercial plug-ins through `vst3_plugin_open_editor`.

## Lessons baked into the host

Behaviours that each cost a debugging session, and that the host now handles for you:

- **The host owns its channel-pointer arrays.** Plug-ins may modify the channel pointers they are
  given while processing (the SDK's `ProcessDataSlicer` advances every pointer per slice), so the
  host copies the caller's pointers into its own arrays. Passing the same array for inputs and
  outputs is therefore safe.
- **A `setBusArrangements` refusal is not decisive.** Several shipping plug-ins return
  `kResultFalse` while already being in exactly the arrangement requested. The host treats the
  *queried* arrangement as authoritative, and still reports `VST3_ERR_UNSUPPORTED_IO` when what is
  actually in effect differs from what was asked for.
- **Editors need an `IPlugFrame`.** Many plug-ins (Native Instruments' among them) call
  `IPlugFrame::resizeView` while attaching their editor, and crash if no frame was set. The host
  sets its own frame on every view before attaching it and resizes the parent window on request.

## License

**GPLv3** — see [`LICENSE`](LICENSE).

`Vst3HostNative.dll` links the Steinberg VST3 SDK, which is dual-licensed GPLv3 / Steinberg
proprietary, and this project takes the GPLv3 option. Anything that links it — the managed
`NetPlugHost` assembly and any application that uses it — inherits GPLv3 obligations when
distributed. [`native/README.md`](native/README.md) covers what that means for consumers, and what a
non-GPL distribution would require.

VST is a registered trademark of Steinberg Media Technologies GmbH.
