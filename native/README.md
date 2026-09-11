# Vst3HostNative

Native VST3 host DLL. Implements the flat C ABI in [`include/vst3_host_c.h`](include/vst3_host_c.h),
which is pinned by [`../ABI_SPEC.md`](../ABI_SPEC.md) §3–§6.

**Windows x64 only.** There is no x86 or cross-platform build (ABI_SPEC.md §9).

## License

This build links the Steinberg VST3 SDK, which is dual-licensed **GPLv3 / Steinberg proprietary**.

**This project takes the GPLv3 option.** `Vst3HostNative.dll` is therefore distributed under the
GNU General Public License v3. Anything that links it — including the managed `NetPlugHost`
assembly and any consuming application such as StreamRecorder — inherits GPLv3 obligations when
distributed.

If a non-GPL distribution is ever needed, that requires registering for and accepting Steinberg's
(free) proprietary license agreement, and this note must be updated to record the change.

The SDK itself is **not committed to this tree** (ABI_SPEC.md §10). It is a git submodule at
`../external/vst3sdk`.

## Prerequisites

- Visual Studio 2022 with the **Desktop development with C++** workload (MSVC x64 toolset +
  Windows SDK)
- CMake 3.20+ (ships with that workload)
- The VST3 SDK submodule:

  ```
  git submodule update --init --recursive
  ```

## Build

```
cmake -S native -B native/build -A x64
cmake --build native/build --config Release
```

Output: `native/build/bin/Vst3HostNative.dll`.

To build against an SDK checkout elsewhere, pass `-DVST3_SDK_DIR=<path>`.

## Notes on the build

Rather than `add_subdirectory` on the SDK's own CMake — which also configures VSTGUI, the sample
plugins, and the validator — this project compiles only the SDK sources a host actually needs
(listed as `SDK_SOURCES` in `CMakeLists.txt`). If a future SDK bump moves or splits one of those
files, configuration fails loudly rather than producing link errors.
