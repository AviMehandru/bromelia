using Bromelia.Domain;
using Bromelia.Foundation;

namespace Bromelia.Engine;

/// <summary>What ProbeStep found: the listing (none when a backup goes on without one), its fingerprint, the LibreDrive
/// detail, and whether the listing is the one the disc was opened with (a backup whose listing failed).</summary>
public sealed record ProbeOutput(Listing? Listing, string? Fingerprint, string? LibreDrive, bool FromSession)
{
    public static JsonValue Encode(ProbeOutput output) => JsonValue.Of(
        ("listing", output.Listing is { } l ? Listing.Encode(l) : JsonValue.Null.Instance),
        ("fingerprint", JsonValue.Of(output.Fingerprint)), ("libreDrive", JsonValue.Of(output.LibreDrive)),
        ("fromSession", JsonValue.Of(output.FromSession)));

    public static ProbeOutput Decode(JsonValue json) => new(
        json["listing"] is JsonValue.Object o ? Listing.Decode(o) : null, json["fingerprint"]?.AsString, json["libreDrive"]?.AsString,
        json["fromSession"]?.AsBool ?? false);
}
