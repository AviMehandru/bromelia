using Bromelia.Domain;
using Bromelia.Foundation;

namespace Bromelia.Engine;

/// <summary>What ProbeStep found: the listing (none when a backup goes on without one), its fingerprint, the LibreDrive
/// detail, whether the listing is the one the disc was opened with (a backup whose listing failed), and MakeMKV's version
/// line (message 1005; null when the run didn't print one), which Seal writes into the archive record.</summary>
public sealed record ProbeOutput(Listing? Listing, string? Fingerprint, string? LibreDrive, bool FromSession, string? MakemkvVersion = null)
{
    public static JsonValue Encode(ProbeOutput output) => JsonValue.Of(
        ("listing", output.Listing is { } l ? Listing.Encode(l) : JsonValue.Null.Instance),
        ("fingerprint", JsonValue.Of(output.Fingerprint)), ("libreDrive", JsonValue.Of(output.LibreDrive)),
        ("fromSession", JsonValue.Of(output.FromSession)), ("makemkvVersion", JsonValue.Of(output.MakemkvVersion)));

    public static ProbeOutput Decode(JsonValue json) => new(
        json["listing"] is JsonValue.Object o ? Listing.Decode(o) : null, json["fingerprint"]?.AsString, json["libreDrive"]?.AsString,
        json["fromSession"]?.AsBool ?? false, json["makemkvVersion"]?.AsString);
}
