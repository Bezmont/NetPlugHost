namespace NetPlugHost;

/// <summary>
/// Parameter capability bits. ABI_SPEC.md §5 (<c>Vst3ParamInfo.flags</c>).
/// </summary>
[Flags]
public enum Vst3ParamFlags
{
    None = 0,
    CanAutomate = 1 << 0,
    IsBypass = 1 << 1,
    IsReadOnly = 1 << 2,
    IsList = 1 << 3,
}

/// <summary>
/// One audio-effect class exposed by a <see cref="Vst3Module"/>. ABI_SPEC.md §5.
/// </summary>
/// <param name="Name">Class (plugin) name.</param>
/// <param name="Category">VST3 subcategories, e.g. <c>"Fx|EQ"</c>.</param>
/// <param name="Vendor">Vendor name.</param>
/// <param name="Version">Plugin version string.</param>
/// <param name="IsAudioEffect">
/// True when the class is an audio-effect component. The native side only enumerates
/// audio-effect classes, so in practice this is always true — it is carried across
/// the ABI so a future non-filtered enumeration would not be a breaking change.
/// </param>
public sealed record Vst3ClassInfo(
    string Name,
    string Category,
    string Vendor,
    string Version,
    bool IsAudioEffect);

/// <summary>
/// One automatable parameter of a <see cref="Vst3Plugin"/>. ABI_SPEC.md §5.
/// </summary>
/// <param name="Id">Pass to <see cref="Vst3Plugin.GetParameter"/> / <see cref="Vst3Plugin.SetParameter"/>.</param>
/// <param name="Title">Display name.</param>
/// <param name="Units">Display units, e.g. <c>"dB"</c>. May be empty.</param>
/// <param name="DefaultNormalized">Default value, normalized to <c>[0,1]</c>.</param>
/// <param name="StepCount">0 for a continuous parameter; &gt;0 for discrete steps.</param>
/// <param name="Flags">Capability bits.</param>
public sealed record Vst3ParamInfo(
    uint Id,
    string Title,
    string Units,
    double DefaultNormalized,
    int StepCount,
    Vst3ParamFlags Flags);
