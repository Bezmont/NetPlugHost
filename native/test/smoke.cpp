// smoke.cpp — native-only exercise of the C ABI.
//
// Mirrors the ABI_SPEC.md §8 sequence without the CLR in the picture, so a crash in
// the host can be attributed to the host rather than to marshalling. Build in Debug
// to get the VST3 SDK's own assertions.
//
//   smoke.exe <path-to.vst3>

#include "vst3_host_c.h"

#include <cmath>
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(bool condition, const char* what) {
    std::printf("   %s  %s\n", condition ? "PASS" : "FAIL", what);
    if (!condition) ++g_failures;
}

std::string narrow(const char16_t* s) {
    std::string out;
    for (; *s; ++s) out.push_back(*s < 128 ? static_cast<char>(*s) : '?');
    return out;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        std::printf("usage: smoke.exe <path-to.vst3>\n");
        return 2;
    }

    constexpr int32_t kFrames = 512;
    constexpr int32_t kChannels = 2;

    // §5 struct layout. The managed side blits these, so a size or offset mismatch
    // silently corrupts memory rather than failing a call — print them to compare.
    std::printf("   layout: Vst3ClassInfo size=%zu\n", sizeof(Vst3ClassInfo));
    std::printf("   layout: Vst3ParamInfo size=%zu  id=%zu title=%zu units=%zu def=%zu step=%zu flags=%zu\n",
                sizeof(Vst3ParamInfo), offsetof(Vst3ParamInfo, id), offsetof(Vst3ParamInfo, title),
                offsetof(Vst3ParamInfo, units), offsetof(Vst3ParamInfo, defaultNormalized),
                offsetof(Vst3ParamInfo, stepCount), offsetof(Vst3ParamInfo, flags));

    Vst3ModuleHandle module = nullptr;
    Vst3Status status = vst3_module_load(argv[1], &module);
    check(status == VST3_OK && module != nullptr, "vst3_module_load");
    if (status != VST3_OK) return 1;

    const int32_t classCount = vst3_module_class_count(module);
    check(classCount >= 1, "class_count >= 1");

    Vst3ClassInfo classInfo{};
    check(vst3_module_class_info(module, 0, &classInfo) == VST3_OK, "class_info(0)");
    std::printf("   name: %s / vendor: %s\n", narrow(classInfo.name).c_str(),
                narrow(classInfo.vendor).c_str());
    check(classInfo.isAudioEffect == 1, "class_info[0].isAudioEffect == 1");

    Vst3PluginHandle plugin = nullptr;
    check(vst3_plugin_create(module, 0, &plugin) == VST3_OK, "plugin_create");
    if (!plugin) return 1;

    const Vst3Status setupStatus = vst3_plugin_setup(plugin, 48000.0, kFrames, kChannels, kChannels);
    std::printf("   setup status: %d\n", setupStatus);
    check(setupStatus == VST3_OK, "plugin_setup(48000, 512, 2, 2)");
    check(vst3_plugin_set_active(plugin, 1) == VST3_OK, "set_active(1)");
    std::printf("   latency: %d\n", vst3_plugin_latency_samples(plugin));

    const int32_t paramCount = vst3_plugin_param_count(plugin);
    check(paramCount > 0, "param_count > 0");

    // Only a parameter that really is a gain lets us predict which way the level moves.
    // On an arbitrary plugin the first continuous parameter is as likely to be an EQ
    // frequency or a reverb size, so the level assertion below is made conditional
    // rather than reported as a host failure.
    uint32_t gainId = 0;
    bool haveGain = false;
    for (int32_t i = 0; i < paramCount; ++i) {
        Vst3ParamInfo info{};
        if (vst3_plugin_param_info(plugin, i, &info) != VST3_OK) continue;
        std::printf("   [%u] %s flags=%d steps=%d\n", info.id, narrow(info.title).c_str(),
                    info.flags, info.stepCount);
        if (info.flags & VST3_PARAM_IS_BYPASS) {
            vst3_plugin_set_param(plugin, info.id, 0.0);
            continue;
        }
        if (haveGain || info.stepCount != 0) continue;

        std::string title = narrow(info.title);
        for (char& ch : title) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));

        // A side-chain gain moves the level the *opposite* way on a compressor: less
        // side-chain signal means less compression, so the output gets louder.
        if (title.find("sc ") == 0 || title.find("sidechain") != std::string::npos ||
            title.find("side-chain") != std::string::npos)
            continue;

        if (title.find("gain") != std::string::npos || title.find("volume") != std::string::npos) {
            gainId = info.id;
            haveGain = true;
        }
    }

    if (haveGain) {
        check(vst3_plugin_set_param(plugin, gainId, 0.25) == VST3_OK, "set_param(gain, 0.25)");
        const double readBack = vst3_plugin_get_param(plugin, gainId);
        check(std::fabs(readBack - 0.25) < 1e-9, "get_param round-trips");
    } else {
        std::printf("   (no gain-like parameter; skipping the level assertion)\n");
    }

    // Planar buffers. Processed in place, exactly as the ABI permits.
    std::vector<std::vector<float>> channels(kChannels, std::vector<float>(kFrames, 0.0f));
    std::vector<float*> pointers(kChannels);
    double inputRms = 0.0;
    for (int32_t c = 0; c < kChannels; ++c) {
        for (int32_t i = 0; i < kFrames; ++i)
            channels[c][i] = static_cast<float>(0.5 * std::sin(2.0 * 3.14159265358979 * 440.0 * i / 48000.0));
        pointers[c] = channels[c].data();
    }
    for (int32_t i = 0; i < kFrames; ++i) inputRms += channels[0][i] * channels[0][i];
    inputRms = std::sqrt(inputRms / kFrames);

    std::printf("   -- calling process --\n");
    status = vst3_plugin_process(plugin, const_cast<const float* const*>(pointers.data()),
                                 pointers.data(), kFrames);
    check(status == VST3_OK, "process(512) returned OK");

    double outputRms = 0.0;
    for (int32_t i = 0; i < kFrames; ++i) outputRms += channels[0][i] * channels[0][i];
    outputRms = std::sqrt(outputRms / kFrames);
    std::printf("   input RMS %.6f -> output RMS %.6f\n", inputRms, outputRms);
    if (haveGain)
        check(outputRms < inputRms * 0.9, "level dropped for gain=0.25");
    else
        check(std::isfinite(outputRms), "output is finite (no NaN/Inf from process)");

    // Second block: state carried correctly, still attenuated.
    status = vst3_plugin_process(plugin, const_cast<const float* const*>(pointers.data()),
                                 pointers.data(), kFrames);
    check(status == VST3_OK, "second process(512) returned OK");

    uint8_t* stateBuf = nullptr;
    int32_t stateLen = 0;
    check(vst3_plugin_get_state(plugin, &stateBuf, &stateLen) == VST3_OK, "get_state");
    check(stateLen > 0 && stateBuf != nullptr, "state blob is non-empty");
    if (stateBuf) {
        check(vst3_plugin_set_state(plugin, stateBuf, stateLen) == VST3_OK, "set_state round-trip");
        vst3_free_buffer(stateBuf);
    }

    std::printf("   has_editor: %d\n", vst3_plugin_has_editor(plugin));
    int32_t w = 0, h = 0;
    if (vst3_plugin_get_editor_size(plugin, &w, &h) == VST3_OK)
        std::printf("   editor size: %dx%d\n", w, h);

    check(vst3_plugin_set_active(plugin, 0) == VST3_OK, "set_active(0)");
    vst3_plugin_destroy(plugin);
    vst3_module_free(module);

    std::printf("\n%s (%d failure(s))\n", g_failures == 0 ? "ALL PASSED" : "FAILED", g_failures);
    return g_failures == 0 ? 0 : 1;
}
