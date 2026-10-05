using System.Collections.Generic;

namespace Bromelia.Domain;

/// <summary>The apprise command's arguments (after <c>apprise</c> or <c>python -m apprise</c>) and environment. The URL
/// goes in APPRISE_URLS, never on the command line: it holds tokens (Telegram, Pushover), and a command line is visible
/// to every user of the machine (ps, /proc/*/cmdline, a Docker host).</summary>
public static class AppriseArgs
{
    /// <summary><c>-t title -b body</c>.</summary>
    public static List<string> Build(string title, string body) => new() { "-t", title, "-b", body };

    /// <summary>APPRISE_URLS = the URL.</summary>
    public static Dictionary<string, string> Environment(string url) => new() { ["APPRISE_URLS"] = url };
}
