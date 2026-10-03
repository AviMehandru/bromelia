using System.Collections.Generic;

namespace Bromelia.Domain;

/// <summary>One file a job will write, for <see cref="Layouts.Paths"/>: its role, title or episode, the tokens of
/// that file (track, episode, episodeNumber, episodeTitle, original, n, …), whether it's a movie's main feature,
/// and its extension (<c>.mkv</c>; empty for a folder).</summary>
public sealed record PlannedOutput(
    PathRole Role,
    IReadOnlyDictionary<string, string> Values,
    int? Title = null,
    int? Episode = null,
    bool MainFeature = false,
    string Extension = "");
