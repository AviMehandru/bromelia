using System.Collections.Generic;
using System.Linq;
using Bromelia.Foundation;

namespace Bromelia.Engine;

/// <summary>What ReconcileStep found: each chosen title of the opened listing → its number in the listing read now.</summary>
public sealed record ReconcileOutput(IReadOnlyDictionary<int, int> TitleMap)
{
    /// <summary><c>{"titleMap": [[opened, now], …]}</c>, by the opened number.</summary>
    public static JsonValue Encode(ReconcileOutput output) => JsonValue.Of(("titleMap",
        JsonValue.Of(output.TitleMap.OrderBy(kv => kv.Key).Select(kv => JsonValue.Of(new[] { JsonValue.Of(kv.Key), JsonValue.Of(kv.Value) })))));

    public static ReconcileOutput Decode(JsonValue json)
    {
        var map = new Dictionary<int, int>();
        foreach (var pair in json["titleMap"]?.AsArray ?? new List<JsonValue>())
            if (pair.AsArray is { Count: 2 } p && p[0].AsInteger is { } from && p[1].AsInteger is { } to) map[(int)from] = (int)to;
        return new ReconcileOutput(map);
    }
}
