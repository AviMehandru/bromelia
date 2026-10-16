using System.Collections.Generic;
using System.Linq;
using Bromelia.Domain;
using Bromelia.Foundation;

namespace Bromelia.Engine;

/// <summary>The disc in a drive (dtos.json Medium): its generation (the drive's discs counted since the engine
/// started), label, type (dvd, bd, hddvd or disc, from MakeMKV's flags), MakeMKV's type text, flags, what is on it and
/// where the OS mounted it.</summary>
public sealed record MediumDto(long Generation, string Label, string Type, string TypeText, IReadOnlyList<string> Flags, DiscContent Content,
    string? MountPath)
{
    /// <summary>The flags' names, in dtos.json order.</summary>
    public static List<string> FlagNames(DiscFlags flags)
    {
        var names = new List<string>();
        if (flags.DvdFiles) names.Add("dvdFiles");
        if (flags.HdDvdFiles) names.Add("hdDvdFiles");
        if (flags.BlurayFiles) names.Add("blurayFiles");
        if (flags.AacsFiles) names.Add("aacsFiles");
        if (flags.BdsvmFiles) names.Add("bdsvmFiles");
        return names;
    }

    /// <summary>The {type} of a disc with these flags (common.json DiscType).</summary>
    public static string TypeOf(DiscFlags flags) => FormatDetector.Detect(null, flags, null) switch
    {
        DiscFormat.Bluray or DiscFormat.Uhd => "bd",
        DiscFormat.Hddvd => "hddvd",
        DiscFormat.Dvd => "dvd",
        _ => "disc",
    };

    public static JsonValue Json(MediumDto medium) => JsonValue.Of(
        ("generation", JsonValue.Of(medium.Generation)),
        ("label", JsonValue.Of(medium.Label)),
        ("type", JsonValue.Of(medium.Type)),
        ("typeText", JsonValue.Of(medium.TypeText)),
        ("flags", JsonValue.Of(medium.Flags.Select(f => JsonValue.Of(f)))),
        ("content", JsonValue.Of(EnumWire.Name(medium.Content))),
        ("mountPath", JsonValue.Of(medium.MountPath)));
}
