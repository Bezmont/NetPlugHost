using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace NetPlugHost;

/// <summary>
/// Blittable mirror of the native <c>Vst3ClassInfo</c>. ABI_SPEC.md §5.
/// </summary>
[StructLayout(LayoutKind.Sequential)]
internal unsafe struct NativeClassInfo
{
    public fixed char Name[128];
    public fixed char Category[128];
    public fixed char Vendor[128];
    public fixed char Version[64];
    public int IsAudioEffect;
}

/// <summary>
/// Blittable mirror of the native <c>Vst3ParamInfo</c>. ABI_SPEC.md §5.
/// </summary>
[StructLayout(LayoutKind.Sequential)]
internal unsafe struct NativeParamInfo
{
    public uint Id;
    public fixed char Title[128];
    public fixed char Units[128];
    public double DefaultNormalized;
    public int StepCount;
    public int Flags;
}

/// <summary>
/// 1:1 P/Invoke surface for <c>Vst3HostNative.dll</c>. ABI_SPEC.md §3.
/// </summary>
/// <remarks>
/// Every entry point is <c>Cdecl</c> per §2. This type is deliberately a direct transcription of
/// the header — no convenience, no validation. The wrapper types add both.
/// </remarks>
internal static unsafe partial class NativeMethods
{
    private const string Dll = "Vst3HostNative";

    // ---- Module lifecycle ----

    [LibraryImport(Dll, StringMarshalling = StringMarshalling.Utf16)]
    [UnmanagedCallConv(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
    internal static partial Vst3Status vst3_module_load(string path, out IntPtr outModule);

    [LibraryImport(Dll)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int vst3_module_class_count(IntPtr module);

    [LibraryImport(Dll)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial Vst3Status vst3_module_class_info(IntPtr module, int index, out NativeClassInfo outInfo);

    [LibraryImport(Dll)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial void vst3_module_free(IntPtr module);

    // ---- Plugin lifecycle ----

    [LibraryImport(Dll)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial Vst3Status vst3_plugin_create(IntPtr module, int classIndex, out IntPtr outPlugin);

    [LibraryImport(Dll)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial void vst3_plugin_destroy(IntPtr plugin);

    // ---- Processing setup ----

    [LibraryImport(Dll)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial Vst3Status vst3_plugin_setup(IntPtr plugin, double sampleRate, int maxBlockSize,
                                                         int inputChannels, int outputChannels);

    [LibraryImport(Dll)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial Vst3Status vst3_plugin_set_active(IntPtr plugin, int active);

    [LibraryImport(Dll)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int vst3_plugin_latency_samples(IntPtr plugin);

    [LibraryImport(Dll)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial Vst3Status vst3_plugin_reset(IntPtr plugin);

    // ---- Processing (audio thread) ----

    [LibraryImport(Dll)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial Vst3Status vst3_plugin_process(IntPtr plugin, float** inputs, float** outputs, int frames);

    // ---- Parameters ----

    [LibraryImport(Dll)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int vst3_plugin_param_count(IntPtr plugin);

    [LibraryImport(Dll)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial Vst3Status vst3_plugin_param_info(IntPtr plugin, int index, out NativeParamInfo outInfo);

    [LibraryImport(Dll)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial double vst3_plugin_get_param(IntPtr plugin, uint paramId);

    [LibraryImport(Dll)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial Vst3Status vst3_plugin_set_param(IntPtr plugin, uint paramId, double normalizedValue);

    // ---- State ----

    [LibraryImport(Dll)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial Vst3Status vst3_plugin_get_state(IntPtr plugin, out IntPtr outBuf, out int outLen);

    [LibraryImport(Dll)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial Vst3Status vst3_plugin_set_state(IntPtr plugin, byte* buf, int len);

    [LibraryImport(Dll)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial void vst3_free_buffer(IntPtr buf);

    // ---- Editor (UI thread only) ----

    [LibraryImport(Dll)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int vst3_plugin_has_editor(IntPtr plugin);

    [LibraryImport(Dll)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial Vst3Status vst3_plugin_open_editor(IntPtr plugin, IntPtr parentHwnd);

    [LibraryImport(Dll)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial Vst3Status vst3_plugin_get_editor_size(IntPtr plugin, out int outWidth, out int outHeight);

    [LibraryImport(Dll)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial Vst3Status vst3_plugin_close_editor(IntPtr plugin);
}
