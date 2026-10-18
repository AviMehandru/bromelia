using Bromelia.Foundation;

namespace Bromelia.Engine;

/// <summary>A unit that holds this disc already: its id, its folder (absolute) and the day it was committed
/// (YYYY-MM-DD, UTC).</summary>
public sealed record ArchivedMatch(Id UnitId, string Folder, string Day)
{
    public static JsonValue Encode(ArchivedMatch match) => JsonValue.Of(("unitId", JsonValue.Of(match.UnitId.Value)),
        ("folder", JsonValue.Of(match.Folder)), ("day", JsonValue.Of(match.Day)));

    public static ArchivedMatch? Decode(JsonValue? json) => json is JsonValue.Object
        ? new ArchivedMatch(new Id(json["unitId"]?.AsString ?? ""), json["folder"]?.AsString ?? "", json["day"]?.AsString ?? "")
        : null;
}
