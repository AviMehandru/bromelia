namespace Bromelia.Domain;

/// <summary>A path relative to the library (folders separated by <c>/</c>), with its role and the title or
/// episode it holds (the API's PlannedPath).</summary>
public sealed record PlannedPath(string Path, PathRole Role, int? Title = null, int? Episode = null);
