using System.Globalization;
using System.Net.Http;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;
using System.Text.Json.Nodes;
using Bromelia.Core.Config;
using Bromelia.Core.Logic;
using Bromelia.Core.Robot;

namespace Bromelia.Core.Engine;

/// <summary>Sends job notifications to webhooks, ntfy, Discord, Slack, or any Apprise URL (with the apprise command).</summary>
public static class NotificationSender
{
    /// <summary>An HTTP POST (Url, headers, body) or an Apprise URL.</summary>
    public sealed record Delivery(Uri? Url, Dictionary<string, string> Headers, byte[] Body, string? AppriseUrl);

    static byte[] Json(object o) => JsonSerializer.SerializeToUtf8Bytes(o);

    /// <summary>How <paramref name="target"/> is reached, or null for an empty or malformed URL.</summary>
    public static Delivery? For(string target, string title, string body, string status)
    {
        var raw = target.Trim();
        var colon = raw.IndexOf("://", StringComparison.Ordinal);
        if (colon <= 0) return null;
        var scheme = raw[..colon].ToLowerInvariant();
        var tags = status == "success" ? "white_check_mark" : "warning";
        switch (scheme)
        {
            case "http" or "https":
                if (!Uri.TryCreate(raw, UriKind.Absolute, out var url)) return null;
                var host = url.Host.ToLowerInvariant();
                if ((host.EndsWith("discord.com") || host.EndsWith("discordapp.com")) && url.AbsolutePath.Contains("/api/webhooks/"))
                    return new(url, new() { ["Content-Type"] = "application/json" }, Json(new Dictionary<string, string> { ["content"] = $"**{title}**\n{body}" }), null);
                if (host == "hooks.slack.com")
                    return new(url, new() { ["Content-Type"] = "application/json" }, Json(new Dictionary<string, string> { ["text"] = $"*{title}*\n{body}" }), null);
                if (host == "ntfy.sh")
                    return new(url, new() { ["Title"] = title, ["Tags"] = tags }, Encoding.UTF8.GetBytes(body), null);
                return new(url, new() { ["Content-Type"] = "application/json" },
                    Json(new Dictionary<string, string> { ["app"] = "Bromelia", ["body"] = body, ["status"] = status, ["title"] = title }), null);
            case "ntfy" or "ntfys":
                // Apprise's form: ntfy://topic (ntfy.sh), ntfy://host/topic, ntfys://host/topic.
                var rest = raw[(colon + 3)..].TrimEnd('/');
                var slash = rest.IndexOf('/');
                var target2 = slash < 0 ? $"https://ntfy.sh/{rest}" : $"{(scheme == "ntfys" ? "https" : "http")}://{rest}";
                return Uri.TryCreate(target2, UriKind.Absolute, out var u)
                    ? new(u, new() { ["Title"] = title, ["Tags"] = tags }, Encoding.UTF8.GetBytes(body), null) : null;
            default:
                return new(null, new(), Array.Empty<byte>(), raw);
        }
    }

    /// <summary>Sends to every enabled target that wants this status. Failures are reported through <paramref name="log"/>.</summary>
    public static async Task SendAsync(IEnumerable<NotificationTarget> targets, string title, string body, string status, Action<string> log)
    {
        using var http = new HttpClient { Timeout = TimeSpan.FromSeconds(20) };
        foreach (var t in targets.Where(t => t.Enabled && !(t.OnlyProblems && status == "success")))
        {
            var d = For(t.Url, title, body, status);
            if (d == null) { log($"Notification: can't use “{t.Url}”"); continue; }
            if (d.AppriseUrl != null)
            {
                var apprise = EpisodeSplitter.FindTool("apprise");
                if (apprise == null) { log($"Notification: “{d.AppriseUrl}” needs the apprise command (pip install apprise)"); continue; }
                try
                {
                    var r = await new ProcessRunner(apprise, new[] { "-t", title, "-b", body, d.AppriseUrl }).RunAsync(_ => { }, TimeSpan.FromMinutes(1));
                    if (r.ExitCode != 0) log($"Notification with apprise failed (exit status {r.ExitCode})");
                }
                catch (Exception e) { log($"Notification with apprise failed: {e.Message}"); }
                continue;
            }
            try
            {
                using var content = new ByteArrayContent(d.Body);
                using var req = new HttpRequestMessage(HttpMethod.Post, d.Url) { Content = content };
                foreach (var (k, v) in d.Headers)
                    if (k == "Content-Type") content.Headers.TryAddWithoutValidation(k, v); else req.Headers.TryAddWithoutValidation(k, v);
                using var resp = await http.SendAsync(req);
                if (!resp.IsSuccessStatusCode) log($"Notification to {d.Url!.Host} failed: HTTP {(int)resp.StatusCode}");
            }
            catch (Exception e) { log($"Notification to {d.Url!.Host} failed: {e.Message}"); }
        }
    }
}

/// <summary>A movie or show found online.</summary>
public sealed record MediaMatch(string Title, int? Year, int? TmdbId, string? ImdbId, string Provider)
{
    /// <summary>Movie or TV show, as the provider lists it.</summary>
    public MediaKind? Kind { get; init; }
    /// <summary>Plot summary (TMDb overview, OMDb plot), for .nfo files.</summary>
    public string Overview { get; init; } = "";
    /// <summary>Poster image URL, or "".</summary>
    public string Poster { get; init; } = "";

    /// <summary>"The Matrix (1999)".</summary>
    public string Label => Year is { } y ? $"{Title} ({y.ToString(CultureInfo.InvariantCulture)})" : Title;

    /// <summary>What to type (or pick) on the disc page to choose this match: "movie/603", "tv/1668" or "tt0133093".</summary>
    public string ChoiceId => TmdbId is { } id ? $"{((Kind ?? MediaKind.Movie) == MediaKind.Tv ? "tv" : "movie")}/{id.ToString(CultureInfo.InvariantCulture)}" : ImdbId ?? "";
}

/// <summary>An episode of a season, as listed online. Aired is yyyy-mm-dd or "".</summary>
public sealed record EpisodeDetails(string Title, string Overview = "", string Aired = "");

/// <summary>A movie or show chosen by its id: a TMDb id (<c>603</c>, <c>tmdb:603</c>, <c>movie/603</c>, <c>tv/1668</c>, a
/// themoviedb.org address; Kind null when it doesn't say) or an IMDb id (<c>tt0133093</c>, an imdb.com address).</summary>
public sealed record OnlineId(int? Tmdb, MediaKind? Kind, string? Imdb)
{
    /// <summary>"TMDb movie/603", "TMDb 603", "tt0133093".</summary>
    public string Label => Imdb ?? "TMDb " + (Kind is { } k ? $"{(k == MediaKind.Tv ? "tv" : "movie")}/{Tmdb}" : $"{Tmdb}");

    public static OnlineId? Parse(string text)
    {
        var t = text.Trim().ToLowerInvariant();
        if (Regex.Match(t, @"tt\d{5,10}") is { Success: true } imdb) return new(null, null, imdb.Value);
        static MediaKind? K(Group g) => g.Success ? (g.Value == "tv" ? MediaKind.Tv : MediaKind.Movie) : null;
        if (Regex.Match(t, @"themoviedb\.org/(movie|tv)/(\d+)") is { Success: true } u)
            return int.TryParse(u.Groups[2].Value, NumberStyles.None, CultureInfo.InvariantCulture, out var n) && n > 0 ? new(n, K(u.Groups[1]), null) : null;
        if (Regex.Match(t, @"^(?:tmdb:)?(?:(movie|tv)/)?(\d{1,9})$") is { Success: true } m
            && int.TryParse(m.Groups[2].Value, NumberStyles.None, CultureInfo.InvariantCulture, out var id) && id > 0)
            return new(id, K(m.Groups[1]), null);
        return null;
    }
}

/// <summary>Looks up movies and shows on TMDb or OMDb: candidates for a name (ranked), a movie or show by its id, and the
/// episodes of a season.</summary>
public static class MetadataLookup
{
    public static string Normalize(string s) => new(s.ToLowerInvariant().Where(char.IsLetterOrDigit).ToArray());

    /// <summary>A year typed after the name: "Inception (2010)" → ("Inception", 2010).</summary>
    public static (string Name, int? Year) SplitYear(string name)
    {
        var t = name.Trim();
        var m = Regex.Match(t, @"^(.*\S)\s*\((\d{4})\)$");
        if (m.Success && int.TryParse(m.Groups[2].Value, NumberStyles.None, CultureInfo.InvariantCulture, out var y) && y is >= 1870 and <= 2100)
            return (m.Groups[1].Value, y);
        return (t, null);
    }

    /// <summary>A request: the URL and, for a TMDb read access token, the bearer token.</summary>
    public sealed record Req(Uri Url, string? Bearer);

    static string Q(string s) => Uri.EscapeDataString(s);

    /// <summary>A TMDb request: an API key (v3) goes into the query, a read access token into a Bearer header.</summary>
    static Req? Tmdb(string path, string query, MetadataConfig config)
    {
        var key = config.ApiKey.Trim();
        if (key.Length == 0) return null;
        var url = $"https://api.themoviedb.org/3/{path}?{query}{(query.Length > 0 ? "&" : "")}language={Q(config.Language.Length > 0 ? config.Language : "en-US")}";
        return key.Length <= 40 ? new(new Uri(url + "&api_key=" + Q(key)), null) : new(new Uri(url), key);
    }

    static Req? Omdb(string query, MetadataConfig config)
    {
        var key = config.ApiKey.Trim();
        return key.Length == 0 ? null : new(new Uri($"https://www.omdbapi.com/?apikey={Q(key)}&{query}"), null);
    }

    static string Y(int y) => y.ToString(CultureInfo.InvariantCulture);

    /// <summary>The search for <paramref name="name"/> (optionally released in <paramref name="year"/>).</summary>
    public static Req? Request(string name, MediaKind kind, MetadataConfig config, int? year = null)
    {
        if (name.Length == 0) return null;
        return config.Provider switch
        {
            MetadataProvider.Tmdb => Tmdb($"search/{(kind == MediaKind.Tv ? "tv" : "movie")}",
                $"query={Q(name)}{(year is { } y ? $"&{(kind == MediaKind.Tv ? "first_air_date_year" : "year")}={Y(y)}" : "")}", config),
            MetadataProvider.Omdb => Omdb($"s={Q(name)}&type={(kind == MediaKind.Tv ? "series" : "movie")}{(year is { } y2 ? $"&y={Y(y2)}" : "")}", config),
            _ => null,
        };
    }

    /// <summary>The movie or show with this id. TMDb ids without a kind use <paramref name="kind"/>; OMDb only takes IMDb ids.</summary>
    public static Req? Request(OnlineId id, MediaKind kind, MetadataConfig config) => (config.Provider, id) switch
    {
        (MetadataProvider.Tmdb, { Tmdb: { } n }) => Tmdb($"{((id.Kind ?? kind) == MediaKind.Tv ? "tv" : "movie")}/{Y(n)}", "", config),
        (MetadataProvider.Tmdb, { Imdb: { } tt }) => Tmdb($"find/{tt}", "external_source=imdb_id", config),
        (MetadataProvider.Omdb, { Imdb: { } tt }) => Omdb($"i={Q(tt)}&plot=full", config),
        _ => null,
    };

    /// <summary>The episodes of <paramref name="season"/> of the show <paramref name="match"/>.</summary>
    public static Req? SeasonRequest(MediaMatch match, int season, MetadataConfig config) => config.Provider switch
    {
        MetadataProvider.Tmdb when match.TmdbId is { } id => Tmdb($"tv/{Y(id)}/season/{Y(season)}", "", config),
        MetadataProvider.Omdb when match.ImdbId is { } tt => Omdb($"i={Q(tt)}&Season={Y(season)}", config),
        _ => null,
    };

    static int? Year(JsonNode? n) => Str(n) is { Length: >= 4 } s && int.TryParse(s[..4], NumberStyles.None, CultureInfo.InvariantCulture, out var y) ? y : null;
    static string? Str(JsonNode? n) => n is JsonValue v && v.TryGetValue<string>(out var s) ? s : null;
    static string Text(JsonNode? n) => Str(n) is { } s && s != "N/A" ? s : "";
    static int? Int(JsonNode? n) => n is JsonValue v && v.TryGetValue<int>(out var i) ? i : null;
    static JsonObject? Obj(string json) { try { return JsonNode.Parse(json) as JsonObject; } catch (JsonException) { return null; } }

    static MediaMatch? TmdbMatch(JsonObject r, MediaKind kind)
    {
        var title = Str(r["title"]) ?? Str(r["name"]);
        if (string.IsNullOrEmpty(title)) return null;
        var imdb = Text(r["imdb_id"]);
        return new(title, Year(r["release_date"] ?? r["first_air_date"]), Int(r["id"]), imdb.Length > 0 ? imdb : null, "TMDb")
        {
            Kind = kind, Overview = Text(r["overview"]),
            Poster = Str(r["poster_path"]) is { } p ? "https://image.tmdb.org/t/p/original" + p : "",
        };
    }

    static MediaMatch? OmdbMatch(JsonObject r, MediaKind? kind)
    {
        if (Str(r["Title"]) is not { Length: > 0 } title) return null;
        var type = Str(r["Type"]);
        return new(title, Year(r["Year"]), null, Str(r["imdbID"]), "OMDb")
        {
            Kind = type == "series" ? MediaKind.Tv : type == "movie" ? MediaKind.Movie : kind, Overview = Text(r["Plot"]), Poster = Text(r["Poster"]),
        };
    }

    /// <summary>The search results, best first: the title matching <paramref name="name"/> (ignoring case and punctuation) scores
    /// 4, the year hint 2 (1 for a year off by one); ties keep the provider's order.</summary>
    public static List<MediaMatch> Candidates(string json, MetadataProvider provider, string name, int? year, MediaKind kind)
    {
        if (Obj(json) is not { } o) return new();
        List<MediaMatch> list;
        if (provider == MetadataProvider.Tmdb)
            list = (o["results"] as JsonArray)?.OfType<JsonObject>().Select(r => TmdbMatch(r, kind)).OfType<MediaMatch>().ToList() ?? new();
        else if (provider == MetadataProvider.Omdb && Str(o["Response"]) == "True")
            list = o["Search"] is JsonArray search ? search.OfType<JsonObject>().Select(r => OmdbMatch(r, kind)).OfType<MediaMatch>().ToList()
                : OmdbMatch(o, kind) is { } one ? new() { one } : new();
        else
            return new();
        var want = Normalize(name);
        int Score(MediaMatch m) => (Normalize(m.Title) == want ? 4 : 0)
            + (year is { } y && m.Year is { } my ? (my == y ? 2 : Math.Abs(my - y) == 1 ? 1 : 0) : 0);
        return list.Select((m, i) => (m, i)).OrderByDescending(x => Score(x.m)).ThenBy(x => x.i).Select(x => x.m).ToList();
    }

    /// <summary>The best search result (see <see cref="Candidates"/>).</summary>
    public static MediaMatch? Parse(string json, MetadataProvider provider, string name, int? year = null, MediaKind kind = MediaKind.Movie) =>
        Candidates(json, provider, name, year, kind).FirstOrDefault();

    /// <summary>A movie or show read by its id: TMDb details or /find, or OMDb details.</summary>
    public static MediaMatch? Details(string json, MetadataProvider provider, MediaKind kind)
    {
        if (Obj(json) is not { } o) return null;
        if (provider == MetadataProvider.Tmdb)
        {
            if (o.ContainsKey("movie_results") || o.ContainsKey("tv_results"))
            {
                // The kind the id belongs to, whatever the disc was taken for.
                var movie = (o["movie_results"] as JsonArray)?.OfType<JsonObject>().FirstOrDefault();
                var show = (o["tv_results"] as JsonArray)?.OfType<JsonObject>().FirstOrDefault();
                if (kind == MediaKind.Tv) return show != null ? TmdbMatch(show, MediaKind.Tv) : movie != null ? TmdbMatch(movie, MediaKind.Movie) : null;
                return movie != null ? TmdbMatch(movie, MediaKind.Movie) : show != null ? TmdbMatch(show, MediaKind.Tv) : null;
            }
            if (!o.ContainsKey("id")) return null;
            return TmdbMatch(o, o.ContainsKey("name") && !o.ContainsKey("title") ? MediaKind.Tv : MediaKind.Movie);
        }
        return provider == MetadataProvider.Omdb && Str(o["Response"]) == "True" ? OmdbMatch(o, kind) : null;
    }

    /// <summary>Episode number → title, plot and air date.</summary>
    public static Dictionary<int, EpisodeDetails> Season(string json, MetadataProvider provider)
    {
        var d = new Dictionary<int, EpisodeDetails>();
        if (Obj(json) is not { } o) return d;
        if (provider == MetadataProvider.Tmdb)
        {
            foreach (var e in (o["episodes"] as JsonArray)?.OfType<JsonObject>() ?? Enumerable.Empty<JsonObject>())
                if (Int(e["episode_number"]) is { } n && Str(e["name"]) is { Length: > 0 } t) d[n] = new(t, Text(e["overview"]), Text(e["air_date"]));
        }
        else if (provider == MetadataProvider.Omdb)
        {
            foreach (var e in (o["Episodes"] as JsonArray)?.OfType<JsonObject>() ?? Enumerable.Empty<JsonObject>())
                if (int.TryParse(Str(e["Episode"]), NumberStyles.None, CultureInfo.InvariantCulture, out var n) && Str(e["Title"]) is { Length: > 0 } t)
                    d[n] = new(t, "", Text(e["Released"]));
        }
        return d;
    }

    static async Task<string> FetchAsync(Req r, MetadataConfig config)
    {
        using var http = new HttpClient { Timeout = TimeSpan.FromSeconds(20) };
        using var req = new HttpRequestMessage(HttpMethod.Get, r.Url);
        if (r.Bearer != null) req.Headers.TryAddWithoutValidation("Authorization", "Bearer " + r.Bearer);
        using var resp = await http.SendAsync(req).ConfigureAwait(false);
        if (!resp.IsSuccessStatusCode) throw new JobException($"{config.Provider.Label()} answered HTTP {(int)resp.StatusCode}");
        return await resp.Content.ReadAsStringAsync().ConfigureAwait(false);
    }

    /// <summary>The candidates for <paramref name="name"/>, best first. A year that finds nothing is dropped; OMDb's best result is
    /// read again for its plot.</summary>
    public static async Task<List<MediaMatch>> SearchAsync(string name, MediaKind kind, int? year, MetadataConfig config)
    {
        var list = new List<MediaMatch>();
        if (Request(name, kind, config, year) is { } r) list = Candidates(await FetchAsync(r, config).ConfigureAwait(false), config.Provider, name, year, kind);
        if (list.Count == 0 && year != null && Request(name, kind, config) is { } r2)
            list = Candidates(await FetchAsync(r2, config).ConfigureAwait(false), config.Provider, name, year, kind);
        if (config.Provider == MetadataProvider.Omdb && list.FirstOrDefault()?.ImdbId is { } tt && Request(new OnlineId(null, null, tt), kind, config) is { } r3)
        {
            try { if (Details(await FetchAsync(r3, config).ConfigureAwait(false), MetadataProvider.Omdb, kind) is { } full) list[0] = full; }
            catch (Exception e) when (e is HttpRequestException or TaskCanceledException or JobException) { }
        }
        return list;
    }

    public static async Task<MediaMatch?> LookupAsync(OnlineId id, MediaKind kind, MetadataConfig config)
    {
        if (Request(id, kind, config) is not { } r)
            throw new JobException($"{config.Provider.Label()} can't look up {id.Label}{(config.Provider == MetadataProvider.Omdb ? " (it takes IMDb ids, tt…)" : "")}");
        return Details(await FetchAsync(r, config).ConfigureAwait(false), config.Provider, kind);
    }

    public static async Task<Dictionary<int, EpisodeDetails>> EpisodesAsync(MediaMatch match, int season, MetadataConfig config) =>
        SeasonRequest(match, season, config) is { } r ? Season(await FetchAsync(r, config).ConfigureAwait(false), config.Provider) : new();
}

/// <summary>Names for Plex / Jellyfin / Emby libraries.</summary>
public static class MediaServerNaming
{
    public const string FolderTemplate = "{libraryFolder}/{name}{releaseYear? ({releaseYear})}";
    public const string MainTemplate = "{name}{releaseYear? ({releaseYear})}";
    public const string EpisodeTemplate = "Season {seasonOr1:2}/{name}{releaseYear? ({releaseYear})} - S{seasonOr1:2}E{episodeNumber:2}{episodeTitle? - {episodeTitle}}";
    public const string OtherTemplate = "Other/{name}{releaseYear? ({releaseYear})} - {track}";
    public const string BackupName = "{name}{releaseYear? ({releaseYear})} - Backup - {format}";

    /// <summary>The file name template for one file: an episode, the main feature of a movie, or another title.</summary>
    public static string FileTemplate(IReadOnlyDictionary<string, string> values, bool isMainFeature)
    {
        if (values.TryGetValue("episodeNumber", out var e) && e.Length > 0) return EpisodeTemplate;
        if (isMainFeature && values.TryGetValue("kind", out var k) && k == "movie") return MainTemplate;
        return OtherTemplate;
    }
}

/// <summary>Kodi / Jellyfin / Emby metadata written next to a media server library: .nfo files and the poster.</summary>
public static class MediaServerMetadata
{
    public const string PosterName = "poster.jpg";

    /// <summary>Files this writes, which SHA256SUMS doesn't list (media servers may rewrite them).</summary>
    public static bool IsMetadataFile(string name) => name.EndsWith(".nfo", StringComparison.OrdinalIgnoreCase) || name == PosterName;

    public static string Escape(string s) => s.Replace("&", "&amp;").Replace("<", "&lt;").Replace(">", "&gt;");

    static string Document(string root, IEnumerable<(string Name, string Value)> elements)
    {
        var sb = new StringBuilder("<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<").Append(root).Append(">\n");
        foreach (var (name, value) in elements.Where(e => e.Value.Length > 0))
        {
            var close = name.StartsWith("uniqueid ", StringComparison.Ordinal) ? "uniqueid" : name;
            sb.Append("  <").Append(name).Append('>').Append(Escape(value)).Append("</").Append(close).Append(">\n");
        }
        return sb.Append("</").Append(root).Append(">\n").ToString();
    }

    /// <summary>movie.nfo / tvshow.nfo: title, year, plot and the ids (the first one is the default).</summary>
    public static string Nfo(MediaMatch m, MediaKind kind)
    {
        var e = new List<(string, string)> { ("title", m.Title), ("year", m.Year?.ToString(CultureInfo.InvariantCulture) ?? ""), ("plot", m.Overview) };
        var ids = new List<(string Type, string Value)>();
        if (m.TmdbId is { } t) ids.Add(("tmdb", t.ToString(CultureInfo.InvariantCulture)));
        if (m.ImdbId is { Length: > 0 } i) ids.Add(("imdb", i));
        for (int n = 0; n < ids.Count; n++) e.Add(($"uniqueid type=\"{ids[n].Type}\"{(n == 0 ? " default=\"true\"" : "")}", ids[n].Value));
        return Document(kind == MediaKind.Tv ? "tvshow" : "movie", e);
    }

    /// <summary>An episode's .nfo.</summary>
    public static string EpisodeNfo(string show, int season, int episode, EpisodeDetails d) => Document("episodedetails", new (string, string)[]
    {
        ("title", d.Title), ("showtitle", show), ("season", season.ToString(CultureInfo.InvariantCulture)),
        ("episode", episode.ToString(CultureInfo.InvariantCulture)), ("plot", d.Overview), ("aired", d.Aired),
    });

    public static async Task DownloadAsync(Uri url, string file)
    {
        using var http = new HttpClient { Timeout = TimeSpan.FromSeconds(60) };
        using var resp = await http.GetAsync(url).ConfigureAwait(false);
        if (!resp.IsSuccessStatusCode) throw new JobException($"HTTP {(int)resp.StatusCode}");
        var data = await resp.Content.ReadAsByteArrayAsync().ConfigureAwait(false);
        if (data.Length == 0) throw new JobException("empty answer");
        using var f = new FileStream(file, FileMode.CreateNew, FileAccess.Write);
        await f.WriteAsync(data).ConfigureAwait(false);
    }
}

public enum DiscContent { Video, Audio, Data, Unknown }

public static class DiscContentRules
{
    /// <summary>The mode for a disc in an automatic or quick rip: the drive's mode for DVDs and Blu-rays, AudioCD /
    /// DataImage for other discs when the drive is set up for them, null to leave the disc alone.</summary>
    public static RipMode? ModeFor(DiscFlags flags, Func<DiscContent> content, DriveConfig drive)
    {
        if ((flags & (DiscFlags.DvdFiles | DiscFlags.BlurayFiles | DiscFlags.HdDvdFiles)) != 0) return drive.Rip.Mode;
        return content() switch
        {
            DiscContent.Audio => drive.Other.RipAudioCDs ? RipMode.AudioCD : null,
            DiscContent.Data => drive.Other.ImageDataDiscs ? RipMode.DataImage : null,
            _ => drive.Rip.Mode,
        };
    }

    /// <summary>A mounted volume with a DVD / Blu-ray structure is a video disc.</summary>
    public static bool HasVideoStructure(string? root) =>
        root != null && Directory.Exists(root) &&
        Directory.EnumerateDirectories(root).Select(Path.GetFileName).Any(n => n != null && n.ToUpperInvariant() is "BDMV" or "VIDEO_TS" or "HVDVD_TS");
}

/// <summary>Tools for discs MakeMKV doesn't handle.</summary>
public static class OtherDiscTools
{
    /// <summary>The audio CD command: the configured one, else cyanrip, else abcde. Runs in the output folder.</summary>
    public static (string Exe, List<string> Args)? AudioCommand(OtherDiscsConfig config, string device)
    {
        var custom = config.AudioCommand.Trim();
        if (custom.Length > 0)
        {
            var parts = ArgumentSplitter.Split(custom).Select(p => TemplateRenderer.Render(p, new Dictionary<string, string> { ["device"] = device })).ToList();
            if (parts.Count == 0) return null;
            var exe = Path.IsPathRooted(parts[0]) ? (File.Exists(parts[0]) ? parts[0] : null) : EpisodeSplitter.FindTool(parts[0]);
            return exe == null ? null : (exe, parts.Skip(1).ToList());
        }
        if (EpisodeSplitter.FindTool("cyanrip") is { } cyanrip) return (cyanrip, new() { "-d", device, "-o", "flac" });
        if (EpisodeSplitter.FindTool("abcde") is { } abcde) return (abcde, new() { "-d", device, "-o", "flac", "-N" });
        return null;
    }

    /// <summary>The path to read a disc from: \\.\E: for a Windows drive letter (<c>E:</c> or <c>E:\</c>), else the device
    /// itself. A longer path such as <c>C:\images\disc.iso</c> is a file, never its whole volume.</summary>
    public static string DevicePath(string device) => DevicePath(device, OperatingSystem.IsWindows());

    public static string DevicePath(string device, bool windows)
    {
        var d = device.Trim();
        bool driveLetter = d.Length >= 2 && char.IsLetter(d[0]) && d[1] == ':' && (d.Length == 2 || (d.Length == 3 && d[2] is '\\' or '/'));
        return windows && driveLetter ? $@"\\.\{char.ToUpperInvariant(d[0])}:" : d;
    }

    /// <summary>Copies a data disc sector by sector into <paramref name="destination"/>. A read error fails the copy (no
    /// silently zero-filled sectors).</summary>
    public static async Task CopyDiscAsync(string device, string destination, Action<long> progress, CancellationToken ct)
    {
        await Task.Run(() =>
        {
            using var src = new FileStream(DevicePath(device), FileMode.Open, FileAccess.Read, FileShare.ReadWrite, 1, FileOptions.None);
            using var dst = new FileStream(destination, FileMode.CreateNew, FileAccess.Write, FileShare.None, 1 << 20);
            var buffer = new byte[2048 * 512];
            long done = 0;
            int n;
            while ((n = src.Read(buffer, 0, buffer.Length)) > 0)
            {
                ct.ThrowIfCancellationRequested();
                dst.Write(buffer, 0, n);
                done += n;
                progress(done);
            }
            dst.Flush(true);
        }, ct);
    }
}
