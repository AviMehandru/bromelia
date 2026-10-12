namespace Bromelia.Engine;

/// <summary>The thing a problem is about: a library, a drive, a job…, by its id.</summary>
public sealed record ProblemScope(ProblemScopeKind Kind, string Id);
