using System.Globalization;
using System.Text.RegularExpressions;

namespace Bromelia.Domain;

/// <summary>A movie or show chosen by its id: a TMDb id (<c>603</c>, <c>tmdb:603</c>, <c>movie/603</c>,
/// <c>tv/1668</c>, a themoviedb.org address; no kind when it doesn't say) or an IMDb id (<c>tt0133093</c>, an
/// imdb.com address).</summary>
public abstract record OnlineId
{
    public sealed record Tmdb(int Id, MediaKind? Kind) : OnlineId;
    public sealed record Imdb(string Id) : OnlineId;

    private static readonly Regex ImdbId = new(@"tt\d{5,10}", RegexOptions.CultureInvariant);
    private static readonly Regex TmdbUrl = new(@"themoviedb\.org/(movie|tv)/(\d+)", RegexOptions.CultureInvariant);
    private static readonly Regex TmdbText = new(@"^(?:tmdb:)?(?:(movie|tv)/)?(\d{1,9})$", RegexOptions.CultureInvariant);
    private static readonly Regex WithYear = new(@"^(.*\S)\s*\((\d{4})\)$", RegexOptions.CultureInvariant);

    /// <summary>The id in <paramref name="text"/> (case and surrounding space ignored); null when there is none.</summary>
    public static OnlineId? Parse(string text)
    {
        var t = MessageCatalog.AsciiLower(text.Trim());
        if (ImdbId.Match(t) is { Success: true } imdb) return new Imdb(imdb.Value);
        static MediaKind? K(Group g) => g.Success ? (g.Value == "tv" ? MediaKind.Tv : MediaKind.Movie) : null;
        if (TmdbUrl.Match(t) is { Success: true } u)
            return int.TryParse(u.Groups[2].Value, NumberStyles.None, CultureInfo.InvariantCulture, out var n) && n > 0 ? new Tmdb(n, K(u.Groups[1])) : null;
        if (TmdbText.Match(t) is { Success: true } m && int.TryParse(m.Groups[2].Value, NumberStyles.None, CultureInfo.InvariantCulture, out var id) && id > 0)
            return new Tmdb(id, K(m.Groups[1]));
        return null;
    }

    /// <summary>A year typed after the name: "Inception (2010)" → Inception, 2010 (years 1870 to 2100); otherwise the
    /// trimmed text and no year.</summary>
    public static NameAndYear SplitYear(string text)
    {
        var t = text.Trim();
        var m = WithYear.Match(t);
        if (m.Success && int.TryParse(m.Groups[2].Value, NumberStyles.None, CultureInfo.InvariantCulture, out var y) && y is >= 1870 and <= 2100)
            return new NameAndYear(m.Groups[1].Value, y);
        return new NameAndYear(t, null);
    }
}
