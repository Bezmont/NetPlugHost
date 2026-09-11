namespace NetPlugHost;

/// <summary>
/// Status codes returned across the native ABI. ABI_SPEC.md §4.
/// <c>0</c> is success; every negative value is an error.
/// </summary>
public enum Vst3Status
{
    Ok = 0,
    InvalidHandle = -1,
    InvalidArg = -2,
    LoadFailed = -3,
    NoAudioClass = -4,
    UnsupportedIo = -5,
    NotSetup = -6,
    State = -7,
    NoEditor = -8,
    Internal = -9,
}

/// <summary>
/// Thrown when a native call returns a negative <see cref="Vst3Status"/>.
/// </summary>
/// <remarks>
/// ABI_SPEC.md §4: outcomes that are expected rather than exceptional — "this plugin has no
/// editor", for instance — are modelled as a <c>bool</c> or empty result instead of an exception.
/// </remarks>
public sealed class Vst3Exception : Exception
{
    public Vst3Status Status { get; }

    public Vst3Exception(Vst3Status status, string operation)
        : base($"{operation} failed: {status} ({(int)status}).")
    {
        Status = status;
    }

    /// <summary>Throws if <paramref name="status"/> is negative.</summary>
    internal static void ThrowIfFailed(Vst3Status status, string operation)
    {
        if (status != Vst3Status.Ok)
            throw new Vst3Exception(status, operation);
    }

    /// <summary>
    /// Validates a native function that returns a count instead of a status: non-negative values
    /// are the result, negative values are a status code.
    /// </summary>
    internal static int CountOrThrow(int result, string operation)
    {
        if (result < 0)
            throw new Vst3Exception((Vst3Status)result, operation);
        return result;
    }
}
