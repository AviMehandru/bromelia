using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Text;

namespace Bromelia.Domain;

/// <summary>Kodi / Jellyfin / Emby .nfo documents, written next to a media server library (was
/// MediaServerMetadata).</summary>
public static class Nfo
{
    private static string Escape(string s) => s.Replace("&", "&amp;").Replace("<", "&lt;").Replace(">", "&gt;");

    /// <summary>The root element and one element per non-empty value; an element name may carry attributes.</summary>
    private static string Document(string root, IEnumerable<(string Name, string Value)> elements)
    {
        var sb = new StringBuilder("<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<").Append(root).Append(">\n");
        foreach (var (name, value) in elements.Where(e => e.Value.Length > 0))
        {
            var close = name.Split(' ')[0];
            sb.Append("  <").Append(name).Append('>').Append(Escape(value)).Append("</").Append(close).Append(">\n");
        }
        return sb.Append("</").Append(root).Append(">\n").ToString();
    }

    /// <summary>Title, year, plot and the ids (the first one is the default).</summary>
    private static string Titled(string root, Candidate m)
    {
        var e = new List<(string, string)> { ("title", m.Title), ("year", m.Year?.ToString(CultureInfo.InvariantCulture) ?? ""), ("plot", m.Overview) };
        var ids = new List<(string Type, string Value)>();
        if (m.TmdbId is { } t) ids.Add(("tmdb", t.ToString(CultureInfo.InvariantCulture)));
        if (m.ImdbId is { Length: > 0 } i) ids.Add(("imdb", i));
        for (int n = 0; n < ids.Count; n++) e.Add(("uniqueid type=\"" + ids[n].Type + "\"" + (n == 0 ? " default=\"true\"" : ""), ids[n].Value));
        return Document(root, e);
    }

    /// <summary>movie.nfo: title, year, plot and the ids (the first one is the default).</summary>
    public static string Movie(Candidate match) => Titled("movie", match);

    /// <summary>tvshow.nfo: as for a movie.</summary>
    public static string Show(Candidate match) => Titled("tvshow", match);

    /// <summary>An episode's .nfo: title, show, season, episode, plot and air date.</summary>
    public static string Episode(string show, int season, int episode, EpisodeDetails details) => Document("episodedetails", new[]
    {
        ("title", details.Title), ("showtitle", show), ("season", season.ToString(CultureInfo.InvariantCulture)),
        ("episode", episode.ToString(CultureInfo.InvariantCulture)), ("plot", details.Plot), ("aired", details.Aired),
    });
}
