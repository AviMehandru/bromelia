using System.Collections.Generic;
using System.Linq;
using Bromelia.Foundation;

namespace Bromelia.Engine;

/// <summary>A title AcquireStep ripped: its index as the plan numbers it (the listing Probe read), the files it made
/// in the job's staging folder (MakeMKV's names), and what MakeMKV said about read errors while ripping it.</summary>
public sealed record AcquiredTitle(int Index, IReadOnlyList<string> Files, IReadOnlyList<string> ReadErrors)
{
    public static JsonValue Encode(AcquiredTitle title) => JsonValue.Of(("index", JsonValue.Of(title.Index)),
        ("files", JsonValue.Of(title.Files.Select(f => JsonValue.Of(f)))), ("readErrors", JsonValue.Of(title.ReadErrors.Select(e => JsonValue.Of(e)))));

    public static AcquiredTitle Decode(JsonValue json) => new((int)(json["index"]?.AsInteger ?? 0),
        (json["files"]?.AsArray ?? new List<JsonValue>()).Select(f => f.AsString ?? "").ToList(),
        (json["readErrors"]?.AsArray ?? new List<JsonValue>()).Select(e => e.AsString ?? "").ToList());
}
