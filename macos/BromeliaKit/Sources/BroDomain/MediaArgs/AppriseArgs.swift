/// The apprise command's arguments (after `apprise` or `python -m apprise`) and environment. The URL goes in APPRISE_URLS,
/// never on the command line: it holds tokens (Telegram, Pushover), and a command line is visible to every user of the
/// machine (ps, /proc/*/cmdline, a Docker host).
public enum AppriseArgs {
    /// `-t title -b body`.
    public static func build(_ title: String, body: String) -> [String] { ["-t", title, "-b", body] }

    /// APPRISE_URLS = the URL.
    public static func environment(_ url: String) -> [String: String] { ["APPRISE_URLS": url] }
}
