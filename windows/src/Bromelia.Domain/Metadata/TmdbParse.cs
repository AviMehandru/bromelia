using System.Collections.Generic;
using System.Linq;
using Bromelia.Foundation;
using static Bromelia.Domain.MetadataJson;

namespace Bromelia.Domain;

/// <summary>TMDb's answers.</summary>
public static class TmdbParse
{
    private static Candidate? Match(JsonValue r, MediaKind kind)
    {
        var title = Str(r["title"]) ?? Str(r["name"]);
        if (string.IsNullOrEmpty(title)) return null;
        var imdb = Text(r["imdb_id"]);
        return new Candidate(title, Year(r["release_date"] ?? r["first_air_date"]), Int(r["id"]), imdb.Length > 0 ? imdb : null, "TMDb", kind,
            Text(r["overview"]), Str(r["poster_path"]) is { } p ? "https://image.tmdb.org/t/p/original" + p : "");
    }

    /// <summary>The results of a search, in TMDb's order.</summary>
    public static List<Candidate> Candidates(byte[] bytes, MediaKind kind) =>
        (Object(bytes)?["results"]?.AsArray ?? new List<JsonValue>()).OfType<JsonValue.Object>().Select(r => Match(r, kind)).OfType<Candidate>().ToList();

    /// <summary>A movie or show read by its id: details, or /find (the kind the id belongs to, whatever the disc
    /// was taken for; <paramref name="kind"/> breaks a tie).</summary>
    public static Candidate? Details(byte[] bytes, MediaKind kind)
    {
        if (Object(bytes) is not { } o) return null;
        if (o["movie_results"] != null || o["tv_results"] != null)
        {
            var movie = o["movie_results"]?.AsArray?.OfType<JsonValue.Object>().FirstOrDefault();
            var show = o["tv_results"]?.AsArray?.OfType<JsonValue.Object>().FirstOrDefault();
            if (kind == MediaKind.Tv) return show != null ? Match(show, MediaKind.Tv) : movie != null ? Match(movie, MediaKind.Movie) : null;
            return movie != null ? Match(movie, MediaKind.Movie) : show != null ? Match(show, MediaKind.Tv) : null;
        }
        if (o["id"] == null) return null;
        return Match(o, o["name"] != null && o["title"] == null ? MediaKind.Tv : MediaKind.Movie);
    }

    /// <summary>Episode number → title, air date and plot.</summary>
    public static Dictionary<int, EpisodeDetails> Season(byte[] bytes)
    {
        var d = new Dictionary<int, EpisodeDetails>();
        foreach (var e in (Object(bytes)?["episodes"]?.AsArray ?? new List<JsonValue>()).OfType<JsonValue.Object>())
            if (Int(e["episode_number"]) is { } n && Str(e["name"]) is { Length: > 0 } t) d[n] = new EpisodeDetails(t, Text(e["air_date"]), Text(e["overview"]));
        return d;
    }

    /// <summary>The id of the show's absolute-order episode group: the first group of type 2; null when there is
    /// none.</summary>
    public static string? AbsoluteGroup(byte[] bytes) =>
        (Object(bytes)?["results"]?.AsArray ?? new List<JsonValue>()).FirstOrDefault(g => Int(g["type"]) == 2) is { } group ? Str(group["id"]) : null;

    /// <summary>Absolute episode number → details: an episode's place across the groups (groups by order, episodes
    /// by order), counting from 1.</summary>
    public static Dictionary<int, EpisodeDetails> AbsoluteEpisodes(byte[] bytes)
    {
        var d = new Dictionary<int, EpisodeDetails>();
        var groups = (Object(bytes)?["groups"]?.AsArray ?? new List<JsonValue>()).OfType<JsonValue.Object>()
            .Select((g, i) => (g, i)).OrderBy(x => Int(x.g["order"]) ?? 0).ThenBy(x => x.i).Select(x => x.g);
        int n = 0;
        foreach (var g in groups)
            foreach (var e in (g["episodes"]?.AsArray ?? new List<JsonValue>()).OfType<JsonValue.Object>()
                         .Select((e, i) => (e, i)).OrderBy(x => Int(x.e["order"]) ?? 0).ThenBy(x => x.i).Select(x => x.e))
            {
                n++;
                if (Str(e["name"]) is { Length: > 0 } t) d[n] = new EpisodeDetails(t, Text(e["air_date"]), Text(e["overview"]));
            }
        return d;
    }
}
