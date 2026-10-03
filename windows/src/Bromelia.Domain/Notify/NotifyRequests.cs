using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using Bromelia.Foundation;

namespace Bromelia.Domain;

/// <summary>How a notification URL is sent: Discord, Slack, ntfy and other webhooks over HTTP; Telegram (one chat),
/// Pushover and Gotify directly; any other Apprise URL with the apprise command.</summary>
public static class NotifyRequests
{
    private static readonly KeyValuePair<string, string> Json = new("Content-Type", "application/json");

    private static Delivery Post(string url, IEnumerable<KeyValuePair<string, string>> headers, byte[] body) =>
        new Delivery.Http(new HttpRequestSpec("POST", url, headers.ToList(), body));

    private static Delivery PostJson(string url, IEnumerable<KeyValuePair<string, string>> headers, params (string, JsonValue)[] members) =>
        Post(url, new[] { Json }.Concat(headers), JsonValue.EncodeCanonical(JsonValue.Of(members)));

    /// <summary>The host of an http(s) URL's authority (without user info or port), lower case; "" when there is
    /// none.</summary>
    private static (string Host, string Path) HostAndPath(string rest)
    {
        int end = rest.IndexOfAny(new[] { '/', '?', '#' });
        var authority = end < 0 ? rest : rest.Substring(0, end);
        var path = end < 0 || rest[end] != '/' ? "" : rest.Substring(end).Split('?', '#')[0];
        var host = authority.Substring(authority.LastIndexOf('@') + 1);
        if (host.StartsWith("[")) host = host.Substring(0, Math.Max(0, host.IndexOf(']') + 1));
        else if (host.IndexOf(':') is var colon and >= 0) host = host.Substring(0, colon);
        return (MessageCatalog.AsciiLower(host), path);
    }

    /// <summary>How <paramref name="url"/> is reached for a notification with this title, body and status; null for
    /// an empty or malformed URL.</summary>
    public static Delivery? Build(string url, string title, string body, StatusWord status)
    {
        var raw = url.Trim();
        int sep = raw.IndexOf("://", StringComparison.Ordinal);
        if (sep <= 0) return null;
        var scheme = MessageCatalog.AsciiLower(raw.Substring(0, sep));
        var rest = raw.Substring(sep + 3);
        bool success = status == StatusWord.Success;
        var ntfyHeaders = new List<KeyValuePair<string, string>> { new("Title", title), new("Tags", success ? "white_check_mark" : "warning") };
        switch (scheme)
        {
            case "http" or "https":
                var (host, path) = HostAndPath(rest);
                if (host.Length == 0) return null;
                if ((host.EndsWith("discord.com", StringComparison.Ordinal) || host.EndsWith("discordapp.com", StringComparison.Ordinal)) && path.Contains("/api/webhooks/"))
                    return PostJson(raw, Array.Empty<KeyValuePair<string, string>>(), ("content", JsonValue.Of("**" + title + "**\n" + body)));
                if (host == "hooks.slack.com")
                    return PostJson(raw, Array.Empty<KeyValuePair<string, string>>(), ("text", JsonValue.Of("*" + title + "*\n" + body)));
                if (host == "ntfy.sh") return Post(raw, ntfyHeaders, Encoding.UTF8.GetBytes(body));
                return PostJson(raw, Array.Empty<KeyValuePair<string, string>>(), ("app", JsonValue.Of("Bromelia")), ("body", JsonValue.Of(body)),
                    ("status", JsonValue.Of(EnumWire.Name(status))), ("title", JsonValue.Of(title)));
            case "ntfy" or "ntfys":
                // Apprise's form: ntfy://topic (ntfy.sh), ntfy://host/topic, ntfys://host/topic.
                var topic = rest.TrimEnd('/');
                var target = topic.IndexOf('/') < 0 ? "https://ntfy.sh/" + topic : (scheme == "ntfys" ? "https://" : "http://") + topic;
                return Post(target, ntfyHeaders, Encoding.UTF8.GetBytes(body));
            case "tgram" or "pover" or "gotify" or "gotifys":
                return Direct(scheme, rest, title, body, success) ?? new Delivery.Apprise(raw);
            default:
                return new Delivery.Apprise(raw);
        }
    }

    /// <summary>Apprise URLs sent without Python: Telegram with one chat (tgram://bot_token/chat_id), Pushover
    /// (pover://user_key@app_token[/device]) and Gotify (gotify://host[:port][/path]/app_token, gotifys:// for
    /// HTTPS). Null for the other forms, which go to the apprise command.</summary>
    private static Delivery? Direct(string scheme, string rest, string title, string body, bool success)
    {
        var path = rest.Split('/').ToList();
        if (path.Count > 0 && path[^1].Length == 0) path.RemoveAt(path.Count - 1);
        var none = Array.Empty<KeyValuePair<string, string>>();
        switch (scheme)
        {
            case "tgram":
                if (path.Count != 2 || !path[0].Contains(':') || path[1].Length == 0) return null;
                return PostJson("https://api.telegram.org/bot" + path[0] + "/sendMessage", none,
                    ("chat_id", JsonValue.Of(path[1])), ("text", JsonValue.Of(title + "\n" + body)));
            case "pover":
                int at = path.Count is >= 1 and <= 2 ? path[0].IndexOf('@') : -1;
                if (at <= 0 || at == path[0].Length - 1) return null;
                var members = new List<(string, JsonValue)> { ("token", JsonValue.Of(path[0].Substring(at + 1))), ("user", JsonValue.Of(path[0].Substring(0, at))),
                    ("title", JsonValue.Of(title)), ("message", JsonValue.Of(body)) };
                if (path.Count == 2 && path[1].Length > 0) members.Add(("device", JsonValue.Of(path[1])));
                return PostJson("https://api.pushover.net/1/messages.json", none, members.ToArray());
            default:
                if (path.Count < 2 || path[0].Length == 0 || path[^1].Length == 0) return null;
                var url = (scheme == "gotifys" ? "https://" : "http://") + string.Join("/", path.Take(path.Count - 1)) + "/message";
                return PostJson(url, new[] { new KeyValuePair<string, string>("X-Gotify-Key", path[^1]) },
                    ("title", JsonValue.Of(title)), ("message", JsonValue.Of(body)), ("priority", JsonValue.Of(success ? 5 : 8)));
        }
    }

    /// <summary>The enabled targets that want a notification with this status: a target with onlyProblems gets
    /// none on success.</summary>
    public static List<NotifyTarget> TargetsFor(IReadOnlyList<NotifyTarget> targets, StatusWord status) =>
        targets.Where(t => t.Enabled && !(t.OnlyProblems && status == StatusWord.Success)).ToList();
}
