/// The apprise command's arguments (after `apprise` or `python -m apprise`).
public enum AppriseArgs {
    /// `-t title -b body url`.
    public static func build(_ url: String, title: String, body: String) -> [String] { ["-t", title, "-b", body, url] }
}
