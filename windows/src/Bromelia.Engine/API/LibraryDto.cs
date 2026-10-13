using System.Collections.Generic;
using Bromelia.Domain;
using Bromelia.Foundation;

namespace Bromelia.Engine;

/// <summary>A library, as clients see it (dtos.json Library): online, or the reason it isn't; the free space when it
/// is; when its marker was last seen.</summary>
public sealed record LibraryDto(string Id, string Name, string Path, bool Online, BroMessage? Reason, long? FreeBytes, Instant? LastSeenAt)
{
    /// <summary>Optional members left out when absent.</summary>
    public static JsonValue Json(LibraryDto library)
    {
        var m = new List<KeyValuePair<string, JsonValue>>
        {
            new("id", JsonValue.Of(library.Id)),
            new("name", JsonValue.Of(library.Name)),
            new("path", JsonValue.Of(library.Path)),
            new("online", JsonValue.Of(library.Online)),
        };
        if (library.Reason is { } r)
        {
            var reason = new List<KeyValuePair<string, JsonValue>>(((JsonValue.Object)r.ToJson()).Members)
            {
                new("severity", JsonValue.Of(EnumWire.Name(r.Severity))),
            };
            m.Add(new("reason", new JsonValue.Object(reason)));
        }
        if (library.FreeBytes is { } free) m.Add(new("freeBytes", JsonValue.Of(free)));
        if (library.LastSeenAt is { } seen) m.Add(new("lastSeenAt", JsonValue.Of(Instant.Format(seen))));
        return new JsonValue.Object(m);
    }
}
