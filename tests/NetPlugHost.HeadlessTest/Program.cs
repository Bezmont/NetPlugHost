using NetPlugHost;

namespace NetPlugHost.HeadlessTest;

/// <summary>
/// Headless verification of the NetPlugHost stack — ABI_SPEC.md §8.
/// No StreamRecorder involved, no UI, no audio device.
/// </summary>
/// <remarks>
/// Exits 0 when every check passes, 1 otherwise. Takes an optional .vst3 path; with no argument
/// it locates the test fixture built by <c>tests/fixture</c> (see <see cref="PluginLocator"/>).
/// </remarks>
internal static class Program
{
    private const double SampleRate = 48000;
    private const int MaxBlockSize = 512;
    private const int Channels = 2;

    private static int _failures;

    private static int Main(string[] args)
    {
        // This test drives native code that can hard-crash the process. Block-buffered
        // stdout would swallow the output leading up to a crash — exactly the output
        // needed to locate it.
        Console.SetOut(new StreamWriter(Console.OpenStandardOutput()) { AutoFlush = true });

        Console.WriteLine("NetPlugHost headless test — ABI_SPEC.md §8");
        Console.WriteLine();

        string? pluginPath = args.Length > 0 ? args[0] : PluginLocator.FindTestPlugin();
        if (pluginPath is null)
        {
            Console.Error.WriteLine("FATAL: no test plugin found.");
            Console.Error.WriteLine();
            Console.Error.WriteLine("Build the fixture:");
            Console.Error.WriteLine("  cmake -S tests/fixture -B tests/fixture/build -A x64");
            Console.Error.WriteLine("  cmake --build tests/fixture/build --config Release --target again-sample-accurate");
            Console.Error.WriteLine();
            Console.Error.WriteLine("Or pass a .vst3 path as the first argument.");
            return 1;
        }

        Console.WriteLine($"Plugin: {pluginPath}");
        Console.WriteLine();

        try
        {
            RunFullPass(pluginPath, verbose: true);
            RunContractChecks(pluginPath);

            // §8 step 6: no leak/crash under a repeat loop.
            Console.WriteLine();
            Console.WriteLine("-- §8.6 repeat loop (20 load/create/process/destroy cycles) --");
            for (int i = 0; i < 20; i++)
                RunFullPass(pluginPath, verbose: false);
            Pass("20 full cycles completed without crash");
        }
        catch (Exception ex)
        {
            Fail($"unhandled exception: {ex}");
        }

        Console.WriteLine();
        if (_failures == 0)
        {
            Console.WriteLine("RESULT: all checks passed.");
            return 0;
        }

        Console.WriteLine($"RESULT: {_failures} check(s) FAILED.");
        return 1;
    }

    /// <summary>
    /// Checks the §6 semantics the StreamRecorder adapter depends on but that §8's happy path
    /// never exercises: the channel-negotiation contract and block-size bounds.
    /// </summary>
    private static void RunContractChecks(string pluginPath)
    {
        Console.WriteLine();
        Console.WriteLine("-- §6 adapter contract --");

        using var module = Vst3Module.Load(pluginPath);

        // §6.4: the caller either gets exactly the arrangement it asked for, or a hard
        // UnsupportedIo — never a silent substitution, because the adapter's interleave
        // math is sized on the channel count it requested. Both outcomes are conforming;
        // what must never happen is "setup succeeded but the channel count differs".
        using (var mono = module.CreatePlugin(0))
        {
            try
            {
                mono.Setup(SampleRate, MaxBlockSize, 1, 1);
                Check(mono.InputChannels == 1 && mono.OutputChannels == 1,
                      "§6.4 mono setup succeeded and reports exactly 1 in / 1 out");

                // Prove it: a mono block must actually process through.
                mono.SetActive(true);
                ProcessPlanar(mono, MakeSine(64), channelCount: 1);
                Pass("§6.4 mono block processes after a mono setup");
                mono.SetActive(false);
            }
            catch (Vst3Exception ex) when (ex.Status == Vst3Status.UnsupportedIo)
            {
                Pass("§6.4 mono refused with UnsupportedIo (no silent substitution)");
            }
        }

        using var plugin = module.CreatePlugin(0);
        plugin.Setup(SampleRate, MaxBlockSize, Channels, Channels);
        plugin.SetActive(true);

        // §6.5: maxBlockSize is a ceiling, not a fixed size. The adapter chunks arbitrary
        // buffers, so a short final block must be accepted.
        float[] shortBlock = MakeSine(37);
        try
        {
            ProcessOneBlock(plugin, shortBlock);
            Pass("§6.5 short final block (37 frames) accepted");
        }
        catch (Exception ex)
        {
            Fail($"§6.5 short block rejected: {ex.Message}");
        }

        // ...and anything above the ceiling must be refused rather than overrun a buffer.
        try
        {
            ProcessOneBlock(plugin, MakeSine(MaxBlockSize + 1));
            Fail("§6.5 oversized block should have been rejected");
        }
        catch (ArgumentOutOfRangeException)
        {
            Pass("§6.5 block > maxBlockSize rejected");
        }

        // §3: reset clears state without reallocating; a fresh stream may follow.
        plugin.Reset();
        Pass("reset() after processing");

        plugin.SetActive(false);
    }

    /// <summary>Runs §8 steps 1–6 once against <paramref name="pluginPath"/>.</summary>
    private static void RunFullPass(string pluginPath, bool verbose)
    {
        // ---- §8.1: load the bundle, inspect classes ----
        using var module = Vst3Module.Load(pluginPath);

        int classCount = module.ClassCount;
        if (verbose)
        {
            Console.WriteLine("-- §8.1 module load --");
            Check(classCount >= 1, $"class_count >= 1 (got {classCount})");
        }
        if (classCount < 1) return;

        Vst3ClassInfo classInfo = module.GetClassInfo(0);
        if (verbose)
        {
            Check(classInfo.IsAudioEffect, "class_info[0].isAudioEffect == 1");
            Console.WriteLine($"   name    : {classInfo.Name}");
            Console.WriteLine($"   vendor  : {classInfo.Vendor}");
            Console.WriteLine($"   category: {classInfo.Category}");
            Console.WriteLine($"   version : {classInfo.Version}");
        }

        // ---- §8.2: instantiate, set up, activate ----
        using var plugin = module.CreatePlugin(0);
        plugin.Setup(SampleRate, MaxBlockSize, Channels, Channels);
        plugin.SetActive(true);

        if (verbose)
        {
            Console.WriteLine();
            Console.WriteLine("-- §8.2 setup --");
            Pass($"setup(48000, 512, 2, 2) + set_active(1)");
            Console.WriteLine($"   latency : {plugin.LatencySamples} samples/channel");
            Check(plugin.LatencySamples >= 0, "latency_samples >= 0");
        }

        // ---- §8.3: enumerate params, drive a gain-like one ----
        IReadOnlyList<Vst3ParamInfo> parameters = plugin.GetParameters();
        if (verbose)
        {
            Console.WriteLine();
            Console.WriteLine("-- §8.3 parameters --");
            Check(parameters.Count > 0, $"param_count > 0 (got {parameters.Count})");
            foreach (Vst3ParamInfo p in parameters.Take(8))
                Console.WriteLine($"   [{p.Id}] {p.Title} ({p.Units}) def={p.DefaultNormalized:F3} steps={p.StepCount} flags={p.Flags}");
        }

        Vst3ParamInfo? gain = PickGainParameter(parameters);
        if (gain is null)
        {
            if (verbose) Fail("no gain-like parameter found to drive");
            return;
        }

        // Make sure a bypass parameter isn't masking the effect.
        foreach (Vst3ParamInfo p in parameters.Where(p => p.Flags.HasFlag(Vst3ParamFlags.IsBypass)))
            plugin.SetParameter(p.Id, 0.0);

        const double TestGain = 0.25;  // clearly non-unity
        plugin.SetParameter(gain.Id, TestGain);

        if (verbose)
        {
            double readBack = plugin.GetParameter(gain.Id);
            Check(Math.Abs(readBack - TestGain) < 1e-6,
                  $"get_param round-trips set_param on '{gain.Title}' (set {TestGain}, read {readBack:F6})");
        }

        // ---- §8.4: push a known signal through, assert it changed ----
        float[] reference = MakeSine(MaxBlockSize);

        float[] block1 = ProcessOneBlock(plugin, reference);
        float[] block2 = ProcessOneBlock(plugin, reference);

        if (verbose)
        {
            Console.WriteLine();
            Console.WriteLine("-- §8.4 process --");

            double inRms = Rms(reference);
            double out1Rms = Rms(block1);
            double out2Rms = Rms(block2);

            Console.WriteLine($"   input RMS : {inRms:F6}");
            Console.WriteLine($"   block1 RMS: {out1Rms:F6}");
            Console.WriteLine($"   block2 RMS: {out2Rms:F6}");

            Check(out1Rms > 0.0, "output is not silent");
            // Gain was set to 0.25 of full — the level must have dropped, not just wobbled.
            Check(out1Rms < inRms * 0.9,
                  $"output level dropped as expected for gain={TestGain} ({out1Rms:F6} < {inRms * 0.9:F6})");

            // §8.4: a second identical block must be consistent — state carried correctly.
            // The first block may ramp the parameter in, so blocks 1 and 2 can legitimately
            // differ; what must hold is that block 2 is steady-state and still attenuated.
            Check(out2Rms < inRms * 0.9, "second identical block is consistently attenuated");
            Check(!AllClose(reference, block1), "output differs from input");
        }

        // ---- §8.5: state round-trip ----
        byte[] state = plugin.GetState();
        if (verbose)
        {
            Console.WriteLine();
            Console.WriteLine("-- §8.5 state --");
            Check(state.Length > 0, $"get_state returned a non-empty blob ({state.Length} bytes)");
        }

        plugin.SetState(state);
        if (verbose) Pass("set_state accepted the blob back");

        // ---- §8.6: orderly teardown (using-scopes handle destroy/free) ----
        plugin.SetActive(false);
    }

    /// <summary>
    /// Picks a parameter whose level the test can assert on: prefer one named like a gain,
    /// else the first automatable continuous non-bypass parameter.
    /// </summary>
    private static Vst3ParamInfo? PickGainParameter(IReadOnlyList<Vst3ParamInfo> parameters)
    {
        static bool Usable(Vst3ParamInfo p) =>
            !p.Flags.HasFlag(Vst3ParamFlags.IsBypass) &&
            !p.Flags.HasFlag(Vst3ParamFlags.IsReadOnly);

        Vst3ParamInfo? named = parameters.FirstOrDefault(p =>
            Usable(p) && (p.Title.Contains("gain", StringComparison.OrdinalIgnoreCase) ||
                          p.Title.Contains("volume", StringComparison.OrdinalIgnoreCase) ||
                          p.Title.Contains("level", StringComparison.OrdinalIgnoreCase)));
        if (named is not null) return named;

        return parameters.FirstOrDefault(p => Usable(p) && p.StepCount == 0);
    }

    /// <summary>
    /// Deinterleaves a mono reference into <see cref="Channels"/> planar buffers, processes one
    /// block in place, and returns the result of channel 0.
    /// </summary>
    private static float[] ProcessOneBlock(Vst3Plugin plugin, float[] reference)
        => ProcessPlanar(plugin, reference, Channels);

    /// <inheritdoc cref="ProcessOneBlock"/>
    private static unsafe float[] ProcessPlanar(Vst3Plugin plugin, float[] reference, int channelCount)
    {
        int frames = reference.Length;
        var channels = new float[channelCount][];
        for (int c = 0; c < channelCount; c++)
            channels[c] = (float[])reference.Clone();

        // Pin every channel, then hand the plugin the array of base pointers.
        // In-place is allowed by the ABI, so inputs and outputs are the same arrays.
        var handles = new System.Runtime.InteropServices.GCHandle[channelCount];
        var pointers = new IntPtr[channelCount];
        try
        {
            for (int c = 0; c < channelCount; c++)
            {
                handles[c] = System.Runtime.InteropServices.GCHandle.Alloc(
                    channels[c], System.Runtime.InteropServices.GCHandleType.Pinned);
                pointers[c] = handles[c].AddrOfPinnedObject();
            }

            plugin.Process(pointers, pointers, frames);
        }
        finally
        {
            for (int c = 0; c < channelCount; c++)
                if (handles[c].IsAllocated) handles[c].Free();
        }

        return channels[0];
    }

    private static float[] MakeSine(int frames)
    {
        var buffer = new float[frames];
        const double Freq = 440.0;
        for (int i = 0; i < frames; i++)
            buffer[i] = (float)(0.5 * Math.Sin(2.0 * Math.PI * Freq * i / SampleRate));
        return buffer;
    }

    private static double Rms(float[] buffer)
    {
        double sum = 0;
        foreach (float s in buffer) sum += (double)s * s;
        return Math.Sqrt(sum / buffer.Length);
    }

    private static bool AllClose(float[] a, float[] b)
    {
        if (a.Length != b.Length) return false;
        for (int i = 0; i < a.Length; i++)
            if (Math.Abs(a[i] - b[i]) > 1e-7f) return false;
        return true;
    }

    private static void Check(bool condition, string what)
    {
        if (condition) Pass(what);
        else Fail(what);
    }

    private static void Pass(string what)
    {
        Console.WriteLine($"   PASS  {what}");
    }

    private static void Fail(string what)
    {
        _failures++;
        Console.WriteLine($"   FAIL  {what}");
    }
}

/// <summary>Locates the VST3 bundle built by <c>tests/fixture</c>.</summary>
internal static class PluginLocator
{
    private const string FixtureName = "again-sample-accurate.vst3";

    public static string? FindTestPlugin()
    {
        string? repoRoot = FindRepoRoot();
        if (repoRoot is null) return null;

        string fixtureBuild = Path.Combine(repoRoot, "tests", "fixture", "build");
        if (!Directory.Exists(fixtureBuild)) return null;

        // The SDK drops built plugins under VST3/<config>/; search rather than guess,
        // since the exact layout varies with generator and config.
        return Directory
            .EnumerateDirectories(fixtureBuild, FixtureName, SearchOption.AllDirectories)
            .FirstOrDefault();
    }

    private static string? FindRepoRoot()
    {
        var dir = new DirectoryInfo(AppContext.BaseDirectory);
        while (dir is not null)
        {
            if (File.Exists(Path.Combine(dir.FullName, "ABI_SPEC.md")))
                return dir.FullName;
            dir = dir.Parent;
        }
        return null;
    }
}
