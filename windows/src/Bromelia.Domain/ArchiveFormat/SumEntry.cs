namespace Bromelia.Domain;

/// <summary>A line of SHA256SUMS: a path relative to the unit's folder (folders separated by <c>/</c>) and its
/// SHA-256 in lower-case hex.</summary>
public sealed record SumEntry(string Path, string Sha256);
