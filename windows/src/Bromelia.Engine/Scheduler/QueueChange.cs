using System.Collections.Generic;
using System.Linq;
using Bromelia.Domain;
using Bromelia.Foundation;

namespace Bromelia.Engine;

/// <summary>The data of a queueChanged event (shared/schema/events.json).</summary>
public sealed record QueueChange(Queue Queue, IReadOnlyList<Id> Order)
{
    public static JsonValue Json(QueueChange change) => JsonValue.Of(
        ("queue", JsonValue.Of(EnumWire.Name(change.Queue))),
        ("order", JsonValue.Of(change.Order.Select(id => JsonValue.Of(id.Value)))));
}
