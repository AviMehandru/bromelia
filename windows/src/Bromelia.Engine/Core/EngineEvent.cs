using Bromelia.Foundation;

namespace Bromelia.Engine;

/// <summary>One event: its seq, when it was published, its type, and its DTO as JSON (shared/schema/events.json).</summary>
public sealed record EngineEvent(long Seq, Instant At, EngineEventType Type, JsonValue Data)
{
    /// <summary>{seq, at, type, data}: the envelope clients get.</summary>
    public static JsonValue Json(EngineEvent @event) => JsonValue.Of(
        ("seq", JsonValue.Of(@event.Seq)),
        ("at", JsonValue.Of(Instant.Format(@event.At))),
        ("type", JsonValue.Of(EnumWire.Name(@event.Type))),
        ("data", @event.Data));
}
