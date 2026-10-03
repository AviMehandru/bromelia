namespace Bromelia.Domain;

/// <summary>Where a disc's episode numbering continues: the last episode before it, and where that was found
/// (<c>episodes.how.previousDisc</c>'s source: a folder).</summary>
public sealed record PreviousEpisode(int LastEpisode, string Source);
