namespace Bromelia.Domain;

/// <summary>A TV disc whose episode numbering may continue another's: the show's name (after the lookup), the
/// title of its label, its place in the set, and the highest episode already in its media-server season folder
/// (with the folder, for the log).</summary>
public sealed record ContinuationQuery(
    string Name,
    string LabelTitle,
    int? Season,
    int? Part,
    int? Volume,
    int Disc,
    string? SeasonFolder = null,
    int? SeasonFolderHighest = null);
