namespace NetPlugHost;

/// <summary>
/// A loaded <c>.vst3</c> bundle. ABI_SPEC.md §3 (module lifecycle).
/// </summary>
/// <remarks>
/// <para>
/// Enumerates the bundle's audio-effect classes and instantiates them as <see cref="Vst3Plugin"/>.
/// </para>
/// <para>
/// <b>Ownership:</b> every plugin created from a module must be disposed before the module is.
/// Disposing a module that still has live plugins throws rather than unloading the bundle out
/// from under them — in native terms that would be a use-after-free, not an exception.
/// </para>
/// <para><b>Threading:</b> UI/control thread only (§6.1).</para>
/// </remarks>
public sealed class Vst3Module : IDisposable
{
    private IntPtr _handle;
    private readonly List<Vst3Plugin> _plugins = [];

    private Vst3Module(IntPtr handle, string path)
    {
        _handle = handle;
        Path = path;
    }

    /// <summary>Absolute path the bundle was loaded from.</summary>
    public string Path { get; }

    /// <summary>
    /// Loads a <c>.vst3</c> bundle from an absolute path.
    /// </summary>
    /// <exception cref="Vst3Exception">
    /// <see cref="Vst3Status.LoadFailed"/> if the bundle is missing, not a valid VST3, or has no factory.
    /// </exception>
    public static Vst3Module Load(string path)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(path);

        // The native loader wants an absolute path; resolving here gives a clearer
        // failure than a bare LoadFailed from a relative path that almost worked.
        string full = System.IO.Path.GetFullPath(path);

        Vst3Exception.ThrowIfFailed(NativeMethods.vst3_module_load(full, out IntPtr handle), nameof(Load));
        if (handle == IntPtr.Zero)
            throw new Vst3Exception(Vst3Status.LoadFailed, nameof(Load));

        return new Vst3Module(handle, full);
    }

    /// <summary>Number of audio-effect classes the bundle exposes.</summary>
    public int ClassCount
    {
        get
        {
            ThrowIfDisposed();
            return Vst3Exception.CountOrThrow(NativeMethods.vst3_module_class_count(_handle), nameof(ClassCount));
        }
    }

    /// <summary>Describes the audio-effect class at <paramref name="index"/>.</summary>
    public unsafe Vst3ClassInfo GetClassInfo(int index)
    {
        ThrowIfDisposed();
        ArgumentOutOfRangeException.ThrowIfNegative(index);

        Vst3Exception.ThrowIfFailed(
            NativeMethods.vst3_module_class_info(_handle, index, out NativeClassInfo native),
            nameof(GetClassInfo));

        return new Vst3ClassInfo(
            NativeString.Read(native.Name, 128),
            NativeString.Read(native.Category, 128),
            NativeString.Read(native.Vendor, 128),
            NativeString.Read(native.Version, 64),
            native.IsAudioEffect != 0);
    }

    /// <summary>All audio-effect classes in the bundle, in factory order.</summary>
    public IReadOnlyList<Vst3ClassInfo> GetClasses()
    {
        int count = ClassCount;
        var result = new List<Vst3ClassInfo>(count);
        for (int i = 0; i < count; i++)
            result.Add(GetClassInfo(i));
        return result;
    }

    /// <summary>
    /// Instantiates the audio-effect class at <paramref name="classIndex"/>: creates the processor
    /// and its edit controller and connects them.
    /// </summary>
    public Vst3Plugin CreatePlugin(int classIndex = 0)
    {
        ThrowIfDisposed();
        ArgumentOutOfRangeException.ThrowIfNegative(classIndex);

        Vst3Exception.ThrowIfFailed(
            NativeMethods.vst3_plugin_create(_handle, classIndex, out IntPtr handle),
            nameof(CreatePlugin));
        if (handle == IntPtr.Zero)
            throw new Vst3Exception(Vst3Status.NoAudioClass, nameof(CreatePlugin));

        var plugin = new Vst3Plugin(handle, this);
        _plugins.Add(plugin);
        return plugin;
    }

    internal void OnPluginDisposed(Vst3Plugin plugin) => _plugins.Remove(plugin);

    /// <summary>
    /// Unloads the bundle.
    /// </summary>
    /// <exception cref="InvalidOperationException">
    /// If any plugin created from this module is still alive (§3: they must be destroyed first).
    /// </exception>
    public void Dispose()
    {
        if (_handle == IntPtr.Zero) return;

        if (_plugins.Count > 0)
        {
            throw new InvalidOperationException(
                $"Cannot unload '{Path}': {_plugins.Count} plugin(s) created from this module are " +
                "still alive. Dispose them first (ABI_SPEC.md §3).");
        }

        NativeMethods.vst3_module_free(_handle);
        _handle = IntPtr.Zero;
        GC.SuppressFinalize(this);
    }

    private void ThrowIfDisposed() => ObjectDisposedException.ThrowIf(_handle == IntPtr.Zero, this);
}

/// <summary>
/// Reads null-terminated UTF-16 out of the fixed-size struct arrays the ABI uses (§5).
/// </summary>
internal static unsafe class NativeString
{
    public static string Read(char* buffer, int capacity)
    {
        int length = 0;
        while (length < capacity && buffer[length] != '\0')
            length++;
        return new string(buffer, 0, length);
    }
}
