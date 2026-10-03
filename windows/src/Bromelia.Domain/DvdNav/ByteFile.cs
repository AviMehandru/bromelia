namespace Bromelia.Domain;

/// <summary>A file of a ByteSource: its upper-case name and size in bytes.</summary>
public sealed record ByteFile(string Name, long Size);
