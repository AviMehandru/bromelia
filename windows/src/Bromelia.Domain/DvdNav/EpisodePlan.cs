using System.Collections.Generic;

namespace Bromelia.Domain;

/// <summary>Where the episodes of one title start and end: the first chapter of each episode (1-based), the last
/// chapter of the last one and the rule that found it, the chapters (at least 1 s) played after it, the chapters to
/// split at, the duration and chapter starts in seconds, the chapters kept, each episode's duration, and how each
/// start is reached.</summary>
public sealed record EpisodePlan(
    int Title,
    IReadOnlyList<int> Starts,
    int LastEnd,
    string EndRule,
    IReadOnlyList<int> Tail,
    IReadOnlyList<int> SplitChapters,
    double Duration,
    IReadOnlyList<double> ChapterStarts,
    int KeptChapters,
    IReadOnlyList<double> EpisodeDurations,
    IReadOnlyList<string> Reasons);
