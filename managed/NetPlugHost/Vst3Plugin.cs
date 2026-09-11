namespace NetPlugHost;

/// <summary>
/// One instantiated VST3 plug-in: processor + edit controller. ABI_SPEC.md §3, §6.
/// </summary>
/// <remarks>
/// <para><b>Call order</b> (§3): <see cref="Setup"/> → <see cref="SetActive"/>(true) →
/// <see cref="Process"/>… → <see cref="SetActive"/>(false).</para>
/// <para><b>Threading</b> (§6.1): <see cref="Process"/> is the only member callable from the audio
/// thread. Everything else — editor calls especially — belongs on the UI/control thread.
/// <see cref="SetParameter"/> is the single deliberate exception: it is safe to call from the UI
/// thread while <see cref="Process"/> runs on the audio thread.</para>
/// <para><b>Latency</b> (§6.3): this type does not compensate. <see cref="LatencySamples"/> reports
/// the plugin's delay in samples per channel; aligning an offline render is the caller's job.</para>
/// </remarks>
public sealed class Vst3Plugin : IDisposable
{
    private IntPtr _handle;
    private readonly Vst3Module _module;

    internal Vst3Plugin(IntPtr handle, Vst3Module module)
    {
        _handle = handle;
        _module = module;
    }

    /// <summary>Maximum frames per <see cref="Process"/> call, as passed to <see cref="Setup"/>.</summary>
    public int MaxBlockSize { get; private set; }

    /// <summary>Input channel count negotiated by <see cref="Setup"/>.</summary>
    public int InputChannels { get; private set; }

    /// <summary>Output channel count negotiated by <see cref="Setup"/>.</summary>
    public int OutputChannels { get; private set; }

    // ---- Processing setup ----

    /// <summary>
    /// Negotiates a bus arrangement and prepares 32-bit float processing. Safe to call again to
    /// re-prepare after a format change.
    /// </summary>
    /// <param name="sampleRate">Sample rate in Hz.</param>
    /// <param name="maxBlockSize">Ceiling for <see cref="Process"/>; shorter blocks are fine (§6.5).</param>
    /// <param name="inputChannels">1 (mono) or 2 (stereo).</param>
    /// <param name="outputChannels">1 (mono) or 2 (stereo).</param>
    /// <exception cref="Vst3Exception">
    /// <see cref="Vst3Status.UnsupportedIo"/> if the plugin refuses the requested arrangement. Per
    /// §6.4 the host never silently substitutes a different channel count — callers depending on a
    /// specific layout get a hard failure instead of quietly wrong interleave math.
    /// </exception>
    public void Setup(double sampleRate, int maxBlockSize, int inputChannels, int outputChannels)
    {
        ThrowIfDisposed();
        ArgumentOutOfRangeException.ThrowIfNegativeOrZero(sampleRate);
        ArgumentOutOfRangeException.ThrowIfNegativeOrZero(maxBlockSize);

        Vst3Exception.ThrowIfFailed(
            NativeMethods.vst3_plugin_setup(_handle, sampleRate, maxBlockSize, inputChannels, outputChannels),
            nameof(Setup));

        MaxBlockSize = maxBlockSize;
        InputChannels = inputChannels;
        OutputChannels = outputChannels;
    }

    /// <summary>Turns the plugin on (after a successful <see cref="Setup"/>) or off (flushes).</summary>
    public void SetActive(bool active)
    {
        ThrowIfDisposed();
        Vst3Exception.ThrowIfFailed(
            NativeMethods.vst3_plugin_set_active(_handle, active ? 1 : 0),
            nameof(SetActive));
    }

    /// <summary>
    /// Reported processing latency in samples <b>per channel</b>. Read it after
    /// <see cref="SetActive"/>(true) — it may change across a <see cref="Setup"/>.
    /// </summary>
    public int LatencySamples
    {
        get
        {
            ThrowIfDisposed();
            return Vst3Exception.CountOrThrow(
                NativeMethods.vst3_plugin_latency_samples(_handle), nameof(LatencySamples));
        }
    }

    /// <summary>Clears internal state (tails, filter memory) without reallocating.</summary>
    public void Reset()
    {
        ThrowIfDisposed();
        Vst3Exception.ThrowIfFailed(NativeMethods.vst3_plugin_reset(_handle), nameof(Reset));
    }

    // ---- Processing (audio thread) ----

    /// <summary>
    /// Processes one deinterleaved (planar) block of up to <see cref="MaxBlockSize"/> frames.
    /// In-place is allowed: <paramref name="inputs"/> and <paramref name="outputs"/> may be the
    /// same pointers.
    /// </summary>
    /// <remarks>
    /// Audio-thread entry point, so it allocates nothing and skips the managed-side argument
    /// checking the rest of this type does — the native side validates and returns a status.
    /// </remarks>
    /// <param name="inputs">Array of <see cref="InputChannels"/> pointers to <paramref name="frames"/> floats.</param>
    /// <param name="outputs">Array of <see cref="OutputChannels"/> pointers to <paramref name="frames"/> floats.</param>
    /// <param name="frames">Frame count; must be &lt;= <see cref="MaxBlockSize"/>.</param>
    public unsafe Vst3Status Process(float** inputs, float** outputs, int frames)
        => NativeMethods.vst3_plugin_process(_handle, inputs, outputs, frames);

    /// <summary>
    /// Span overload of <see cref="Process(float**, float**, int)"/> taking channel base pointers.
    /// Throws on failure; prefer the raw overload on the hot path.
    /// </summary>
    public unsafe void Process(ReadOnlySpan<IntPtr> inputs, ReadOnlySpan<IntPtr> outputs, int frames)
    {
        ThrowIfDisposed();
        if (inputs.Length < InputChannels)
            throw new ArgumentException($"Expected {InputChannels} input channel(s).", nameof(inputs));
        if (outputs.Length < OutputChannels)
            throw new ArgumentException($"Expected {OutputChannels} output channel(s).", nameof(outputs));
        if (frames > MaxBlockSize)
            throw new ArgumentOutOfRangeException(nameof(frames), frames,
                $"Exceeds MaxBlockSize ({MaxBlockSize}); chunk the buffer (ABI_SPEC.md §6.5).");

        fixed (IntPtr* pIn = inputs)
        fixed (IntPtr* pOut = outputs)
        {
            Vst3Exception.ThrowIfFailed(
                Process((float**)pIn, (float**)pOut, frames), nameof(Process));
        }
    }

    // ---- Parameters ----

    /// <summary>Number of parameters the edit controller exposes.</summary>
    public int ParameterCount
    {
        get
        {
            ThrowIfDisposed();
            return Vst3Exception.CountOrThrow(
                NativeMethods.vst3_plugin_param_count(_handle), nameof(ParameterCount));
        }
    }

    /// <summary>Describes the parameter at <paramref name="index"/>.</summary>
    public unsafe Vst3ParamInfo GetParameterInfo(int index)
    {
        ThrowIfDisposed();
        ArgumentOutOfRangeException.ThrowIfNegative(index);

        Vst3Exception.ThrowIfFailed(
            NativeMethods.vst3_plugin_param_info(_handle, index, out NativeParamInfo native),
            nameof(GetParameterInfo));

        return new Vst3ParamInfo(
            native.Id,
            NativeString.Read(native.Title, 128),
            NativeString.Read(native.Units, 128),
            native.DefaultNormalized,
            native.StepCount,
            (Vst3ParamFlags)native.Flags);
    }

    /// <summary>All parameters, in controller order.</summary>
    public IReadOnlyList<Vst3ParamInfo> GetParameters()
    {
        int count = ParameterCount;
        var result = new List<Vst3ParamInfo>(count);
        for (int i = 0; i < count; i++)
            result.Add(GetParameterInfo(i));
        return result;
    }

    /// <summary>
    /// Current normalized <c>[0,1]</c> value of <paramref name="paramId"/>, per the edit controller.
    /// </summary>
    /// <exception cref="Vst3Exception">If the handle or parameter id is invalid.</exception>
    public double GetParameter(uint paramId)
    {
        ThrowIfDisposed();
        double value = NativeMethods.vst3_plugin_get_param(_handle, paramId);
        if (double.IsNaN(value))  // §3: NaN is the sentinel for bad handle/id
            throw new Vst3Exception(Vst3Status.InvalidArg, nameof(GetParameter));
        return value;
    }

    /// <summary>
    /// Queues a normalized <c>[0,1]</c> change for <paramref name="paramId"/>.
    /// </summary>
    /// <remarks>
    /// §6.1/§6.2: the one member safe to call from the UI thread while <see cref="Process"/> runs.
    /// The controller sees it immediately; the processor picks it up at the next block.
    /// </remarks>
    public void SetParameter(uint paramId, double normalizedValue)
    {
        ThrowIfDisposed();
        if (normalizedValue is < 0.0 or > 1.0 || double.IsNaN(normalizedValue))
            throw new ArgumentOutOfRangeException(nameof(normalizedValue), normalizedValue,
                "Parameter values crossing the ABI are normalized to [0,1] (ABI_SPEC.md §6.2).");

        Vst3Exception.ThrowIfFailed(
            NativeMethods.vst3_plugin_set_param(_handle, paramId, normalizedValue), nameof(SetParameter));
    }

    // ---- State ----

    /// <summary>
    /// Returns the plugin's full state as an opaque blob. The native buffer is copied and released
    /// before returning, so the caller owns plain managed memory.
    /// </summary>
    public byte[] GetState()
    {
        ThrowIfDisposed();
        Vst3Exception.ThrowIfFailed(
            NativeMethods.vst3_plugin_get_state(_handle, out IntPtr buf, out int len), nameof(GetState));

        if (buf == IntPtr.Zero)
            throw new Vst3Exception(Vst3Status.State, nameof(GetState));

        try
        {
            var managed = new byte[len];
            if (len > 0)
                System.Runtime.InteropServices.Marshal.Copy(buf, managed, 0, len);
            return managed;
        }
        finally
        {
            NativeMethods.vst3_free_buffer(buf);
        }
    }

    /// <summary>Restores a blob previously returned by <see cref="GetState"/>.</summary>
    public unsafe void SetState(ReadOnlySpan<byte> state)
    {
        ThrowIfDisposed();
        fixed (byte* p = state)
        {
            Vst3Exception.ThrowIfFailed(
                NativeMethods.vst3_plugin_set_state(_handle, p, state.Length), nameof(SetState));
        }
    }

    // ---- Editor (UI thread only) ----

    /// <summary>
    /// Whether the plugin provides an HWND-hostable editor view.
    /// </summary>
    /// <remarks>§4: "no editor" is an expected outcome, so this is a bool rather than an exception.</remarks>
    public bool HasEditor
    {
        get
        {
            ThrowIfDisposed();
            return Vst3Exception.CountOrThrow(
                NativeMethods.vst3_plugin_has_editor(_handle), nameof(HasEditor)) != 0;
        }
    }

    /// <summary>
    /// Editor size in pixels. Queryable before <see cref="OpenEditor"/> so the host can size its
    /// window first.
    /// </summary>
    public (int Width, int Height) GetEditorSize()
    {
        ThrowIfDisposed();
        Vst3Exception.ThrowIfFailed(
            NativeMethods.vst3_plugin_get_editor_size(_handle, out int w, out int h), nameof(GetEditorSize));
        return (w, h);
    }

    /// <summary>Creates the plugin's view and attaches it to <paramref name="parentHwnd"/>.</summary>
    public void OpenEditor(IntPtr parentHwnd)
    {
        ThrowIfDisposed();
        if (parentHwnd == IntPtr.Zero)
            throw new ArgumentException("A valid parent HWND is required.", nameof(parentHwnd));

        Vst3Exception.ThrowIfFailed(
            NativeMethods.vst3_plugin_open_editor(_handle, parentHwnd), nameof(OpenEditor));
    }

    /// <summary>Detaches and releases the editor view. Safe when none is open.</summary>
    public void CloseEditor()
    {
        ThrowIfDisposed();
        Vst3Exception.ThrowIfFailed(NativeMethods.vst3_plugin_close_editor(_handle), nameof(CloseEditor));
    }

    // ---- Lifetime ----

    /// <summary>Tears down the plugin, closing the editor first if open.</summary>
    public void Dispose()
    {
        if (_handle == IntPtr.Zero) return;

        NativeMethods.vst3_plugin_destroy(_handle);
        _handle = IntPtr.Zero;
        _module.OnPluginDisposed(this);
        GC.SuppressFinalize(this);
    }

    private void ThrowIfDisposed() => ObjectDisposedException.ThrowIf(_handle == IntPtr.Zero, this);
}
