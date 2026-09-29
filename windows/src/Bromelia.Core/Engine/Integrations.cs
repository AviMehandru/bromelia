using System.Net.Http;
using System.Text;
using System.Text.Json;
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
public sealed record MediaMatch(string Title, int? Year, int? TmdbId, string? ImdbId, string Provider);

/// <summary>Looks up the canonical title and year of a movie or show on TMDb or OMDb.</summary>
public static class MetadataLookup
{
    static string Normalize(string s) => new(s.ToLowerInvariant().Where(char.IsLetterOrDigit).ToArray());

    /// <summary>The request URL and an optional bearer token: TMDb takes an API key (v3) or a read access token.</summary>
    public static (Uri Url, string? Bearer)? Request(string name, MediaKind kind, MetadataConfig config)
    {
        var key = config.ApiKey.Trim();
        if (key.Length == 0 || name.Length == 0) return null;
        var q = Uri.EscapeDataString(name);
        switch (config.Provider)
        {
            case MetadataProvider.Tmdb:
                var url = $"https://api.themoviedb.org/3/search/{(kind == MediaKind.Tv ? "tv" : "movie")}?query={q}&language={Uri.EscapeDataString(config.Language)}";
                return key.Length <= 40 ? (new Uri(url + "&api_key=" + Uri.EscapeDataString(key)), null) : (new Uri(url), key);
            case MetadataProvider.Omdb:
                return (new Uri($"https://www.omdbapi.com/?apikey={Uri.EscapeDataString(key)}&t={q}&type={(kind == MediaKind.Tv ? "series" : "movie")}"), null);
            default:
                return null;
        }
    }

    static int? Year(JsonNode? n) => n?.GetValue<string>() is { Length: >= 4 } s && int.TryParse(s[..4], out var y) ? y : null;

    /// <summary>Picks the result whose title matches <paramref name="name"/> (ignoring case and punctuation), else the first one.</summary>
    public static MediaMatch? Parse(string json, MetadataProvider provider, string name)
    {
        JsonNode? root;
        try { root = JsonNode.Parse(json); } catch (JsonException) { return null; }
        if (root is not JsonObject o) return null;
        if (provider == MetadataProvider.Tmdb)
        {
            var results = (o["results"] as JsonArray)?.OfType<JsonObject>().ToList() ?? new();
            string TitleOf(JsonObject r) => (r["title"] ?? r["name"])?.GetValue<string>() ?? "";
            var pick = results.FirstOrDefault(r => Normalize(TitleOf(r)) == Normalize(name)) ?? results.FirstOrDefault();
            if (pick == null || TitleOf(pick).Length == 0) return null;
            return new(TitleOf(pick), Year(pick["release_date"] ?? pick["first_air_date"]), pick["id"]?.GetValue<int>(), null, "TMDb");
        }
        if (provider == MetadataProvider.Omdb && o["Response"]?.GetValue<string>() == "True" && o["Title"]?.GetValue<string>() is { } title)
            return new(title, Year(o["Year"]), null, o["imdbID"]?.GetValue<string>(), "OMDb");
        return null;
    }

    public static async Task<MediaMatch?> LookupAsync(string name, MediaKind kind, MetadataConfig config)
    {
        if (Request(name, kind, config) is not { } r) return null;
        using var http = new HttpClient { Timeout = TimeSpan.FromSeconds(20) };
        using var req = new HttpRequestMessage(HttpMethod.Get, r.Url);
        if (r.Bearer != null) req.Headers.TryAddWithoutValidation("Authorization", "Bearer " + r.Bearer);
        using var resp = await http.SendAsync(req);
        if (!resp.IsSuccessStatusCode) throw new JobException($"{config.Provider} answered HTTP {(int)resp.StatusCode}");
        return Parse(await resp.Content.ReadAsStringAsync(), config.Provider, name);
    }
}

/// <summary>Names for Plex / Jellyfin / Emby libraries.</summary>
public static class MediaServerNaming
{
    public const string FolderTemplate = "{libraryFolder}/{name}{releaseYear? ({releaseYear})}";
    public const string MainTemplate = "{name}{releaseYear? ({releaseYear})}";
    public const string EpisodeTemplate = "Season {seasonOr1:2}/{name}{releaseYear? ({releaseYear})} - S{seasonOr1:2}E{episodeNumber:2}";
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
