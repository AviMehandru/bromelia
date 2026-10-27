namespace Bromelia.Engine;

/// <summary>A file a job made, as JobDetail lists it (dtos.json JobDetail.files): Seal's (path in the unit folder, size
/// and SHA-256 as read back from disk, role), else Acquire's (path in staging only).</summary>
public sealed record JobFileDto(string Path, long? Size, string? Sha256, string? Role);
