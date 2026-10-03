namespace Bromelia.Domain;

/// <summary>The first and last disc chapter of an episode.</summary>
public readonly record struct ChapterRange(int First, int Last);
