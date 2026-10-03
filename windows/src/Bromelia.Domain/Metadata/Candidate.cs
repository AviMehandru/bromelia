using System.Globalization;

namespace Bromelia.Domain;

/// <summary>A movie or show found online: title, year, TMDb and IMDb ids, the provider ("TMDb" or "OMDb"), movie or
/// TV as listed, plot and poster URL ("" when there is none).</summary>
public sealed record Candidate(string Title, int? Year, int? TmdbId, string? ImdbId, string Provider, MediaKind? Kind = null,
    string Overview = "", string Poster = "")
{
    /// <summary>What to type or pick to choose this candidate: <c>movie/603</c>, <c>tv/1668</c> or <c>tt0133093</c>.</summary>
    public static string Choice(Candidate candidate) => candidate.TmdbId is { } id
        ? (candidate.Kind == MediaKind.Tv ? "tv/" : "movie/") + id.ToString(CultureInfo.InvariantCulture)
        : candidate.ImdbId ?? "";
}
