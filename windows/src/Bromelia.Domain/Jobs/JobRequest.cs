using Bromelia.Foundation;

namespace Bromelia.Domain;

/// <summary>What a job was asked to do (stored as <c>jobs.request</c>). The kind-specific part (the session
/// snapshot of a hand-picked rip, a verify target, a command step) is in <see cref="Details"/>; the engine's
/// phase gives it types.</summary>
public sealed record JobRequest(
    JobKind Kind,
    string Title,
    bool Automatic = false,
    string? DriveId = null,
    long? MediaGeneration = null,
    Id? SessionId = null,
    Id? ParentId = null,
    Id? UnitId = null,
    JsonValue? Options = null,
    JsonValue? Details = null);
