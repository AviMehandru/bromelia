namespace Bromelia.Domain;

/// <summary>An archived disc of a TV show (from its archive record): its place in the set, its highest episode
/// and its folder.</summary>
public sealed record ArchivedDisc(string Name, string LabelTitle, int? Season, int? Part, int? Volume, int? Disc, int? LastEpisode, string Folder);
