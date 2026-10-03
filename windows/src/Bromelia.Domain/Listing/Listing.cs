using System.Collections.Generic;

namespace Bromelia.Domain;

/// <summary>What makemkvcon reported about a disc, ISO or folder: its name (2, else 32), volume name (32), type
/// (from 1), the number of titles it announced, the titles in index order, and every disc attribute.</summary>
public sealed record Listing(
    string Name,
    string VolumeName,
    DiscType Type,
    string TypeText,
    int ReportedTitleCount,
    IReadOnlyList<Title> Titles,
    IReadOnlyDictionary<int, string> Attributes)
{
    public static readonly Listing Empty = new("", "", DiscType.Disc, "", 0, new List<Title>(), new Dictionary<int, string>());
}
