using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using Bromelia.Foundation;
using static Bromelia.Domain.MetadataJson;

namespace Bromelia.Domain;

/// <summary>OMDb's answers.</summary>
public static class OmdbParse
{
    private static Candidate? Match(JsonValue r, MediaKind? kind)
    {
        if (Str(r["Title"]) is not { Length: > 0 } title) return null;
        var type = Str(r["Type"]);
        return new Candidate(title, Year(r["Year"]), null, Str(r["imdbID"]), "OMDb",
            type == "series" ? MediaKind.Tv : type == "movie" ? MediaKind.Movie : kind, Text(r["Plot"]), Text(r["Poster"]));
    }

    private static JsonValue? Answer(byte[] bytes) => Object(bytes) is { } o && Str(o["Response"]) == "True" ? o : null;

    /// <summary>The results of a search (or the one title OMDb answered with), in OMDb's order; <paramref name="kind"/>
    /// when a result doesn't say.</summary>
    public static List<Candidate> Candidates(byte[] bytes, MediaKind? kind)
    {
        if (Answer(bytes) is not { } o) return new List<Candidate>();
        if (o["Search"]?.AsArray is { } search) return search.OfType<JsonValue.Object>().Select(r => Match(r, kind)).OfType<Candidate>().ToList();
        return Match(o, kind) is { } one ? new List<Candidate> { one } : new List<Candidate>();
    }

    /// <summary>A movie or show read by its IMDb id.</summary>
    public static Candidate? Details(byte[] bytes, MediaKind? kind) => Answer(bytes) is { } o ? Match(o, kind) : null;

    /// <summary>Episode number → title and air date (OMDb lists no plots here).</summary>
    public static Dictionary<int, EpisodeDetails> Season(byte[] bytes)
    {
        var d = new Dictionary<int, EpisodeDetails>();
        foreach (var e in (Object(bytes)?["Episodes"]?.AsArray ?? new List<JsonValue>()).OfType<JsonValue.Object>())
            if (int.TryParse(Str(e["Episode"]), NumberStyles.None, CultureInfo.InvariantCulture, out var n) && Str(e["Title"]) is { Length: > 0 } t)
                d[n] = new EpisodeDetails(t, Text(e["Released"]));
        return d;
    }
}
