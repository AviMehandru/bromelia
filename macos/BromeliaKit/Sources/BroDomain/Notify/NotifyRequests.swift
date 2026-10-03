import BroFoundation

/// How a notification URL is sent: Discord, Slack, ntfy and other webhooks over HTTP; Telegram (one chat), Pushover
/// and Gotify directly; any other Apprise URL with the apprise command.
public enum NotifyRequests {
    static func post(_ url: String, _ headers: [(name: String, value: String)], _ body: [UInt8]) -> Delivery {
        .http(request: HttpRequestSpec(method: "POST", url: url, headers: headers, body: body))
    }

    static func postJson(_ url: String, _ headers: [(name: String, value: String)], _ members: [(key: String, value: JsonValue)]) -> Delivery {
        post(url, [("Content-Type", "application/json")] + headers, JsonValue.encodeCanonical(.object(members)))
    }

    /// The host of an http(s) URL's authority (without user info or port), lower case, and its path.
    static func hostAndPath(_ rest: String) -> (host: String, path: String) {
        let s = Array(rest.unicodeScalars)
        let end = s.firstIndex { $0 == "/" || $0 == "?" || $0 == "#" } ?? s.count
        let authority = String(String.UnicodeScalarView(s[..<end]))
        var path = ""
        if end < s.count && s[end] == "/" {
            let tail = s[end...]
            let stop = tail.firstIndex { $0 == "?" || $0 == "#" } ?? s.count
            path = String(String.UnicodeScalarView(s[end..<stop]))
        }
        var host = Substring(authority)
        if let at = host.lastIndex(of: "@") { host = host[host.index(after: at)...] }
        if host.hasPrefix("[") {
            host = host.firstIndex(of: "]").map { host[...$0] } ?? ""
        } else if let colon = host.firstIndex(of: ":") {
            host = host[..<colon]
        }
        return (MessageCatalog.asciiLower(String(host)), path)
    }

    /// How `url` is reached for a notification with this title, body and status; nil for an empty or malformed URL.
    public static func build(_ url: String, title: String, body: String, status: StatusWord) -> Delivery? {
        let raw = url.trimmingCharacters(in: .whitespacesAndNewlines)
        guard let sep = raw.range(of: "://"), sep.lowerBound > raw.startIndex else { return nil }
        let scheme = MessageCatalog.asciiLower(String(raw[..<sep.lowerBound]))
        let rest = String(raw[sep.upperBound...])
        let success = status == .success
        let ntfyHeaders: [(name: String, value: String)] = [("Title", title), ("Tags", success ? "white_check_mark" : "warning")]
        switch scheme {
        case "http", "https":
            let (host, path) = hostAndPath(rest)
            guard !host.isEmpty else { return nil }
            if (host.hasSuffix("discord.com") || host.hasSuffix("discordapp.com")) && path.contains("/api/webhooks/") {
                return postJson(raw, [], [("content", .string("**\(title)**\n\(body)"))])
            }
            if host == "hooks.slack.com" { return postJson(raw, [], [("text", .string("*\(title)*\n\(body)"))]) }
            if host == "ntfy.sh" { return post(raw, ntfyHeaders, Array(body.utf8)) }
            return postJson(raw, [], [("app", .string("Bromelia")), ("body", .string(body)), ("status", .string(status.rawValue)), ("title", .string(title))])
        case "ntfy", "ntfys":
            // Apprise's form: ntfy://topic (ntfy.sh), ntfy://host/topic, ntfys://host/topic.
            var topic = Substring(rest)
            while topic.hasSuffix("/") { topic = topic.dropLast() }
            let target = topic.contains("/") ? (scheme == "ntfys" ? "https://" : "http://") + topic : "https://ntfy.sh/" + topic
            return post(target, ntfyHeaders, Array(body.utf8))
        case "tgram", "pover", "gotify", "gotifys":
            return direct(scheme, rest, title, body, success) ?? .apprise(url: raw)
        default:
            return .apprise(url: raw)
        }
    }

    /// Apprise URLs sent without Python: Telegram with one chat (tgram://bot_token/chat_id), Pushover
    /// (pover://user_key@app_token[/device]) and Gotify (gotify://host[:port][/path]/app_token, gotifys:// for HTTPS).
    /// Nil for the other forms, which go to the apprise command.
    static func direct(_ scheme: String, _ rest: String, _ title: String, _ body: String, _ success: Bool) -> Delivery? {
        var path = rest.split(separator: "/", omittingEmptySubsequences: false).map(String.init)
        if path.last?.isEmpty == true { path.removeLast() }
        switch scheme {
        case "tgram":
            guard path.count == 2, path[0].contains(":"), !path[1].isEmpty else { return nil }
            return postJson("https://api.telegram.org/bot\(path[0])/sendMessage", [], [("chat_id", .string(path[1])), ("text", .string("\(title)\n\(body)"))])
        case "pover":
            guard (1...2).contains(path.count), let at = path[0].firstIndex(of: "@"), at > path[0].startIndex,
                  path[0].index(after: at) < path[0].endIndex else { return nil }
            var members: [(key: String, value: JsonValue)] = [("token", .string(String(path[0][path[0].index(after: at)...]))),
                                                              ("user", .string(String(path[0][..<at]))), ("title", .string(title)), ("message", .string(body))]
            if path.count == 2 && !path[1].isEmpty { members.append(("device", .string(path[1]))) }
            return postJson("https://api.pushover.net/1/messages.json", [], members)
        default:
            guard path.count >= 2, !path[0].isEmpty, let key = path.last, !key.isEmpty else { return nil }
            let url = (scheme == "gotifys" ? "https://" : "http://") + path.dropLast().joined(separator: "/") + "/message"
            return postJson(url, [("X-Gotify-Key", key)], [("title", .string(title)), ("message", .string(body)), ("priority", .integer(success ? 5 : 8))])
        }
    }

    /// The enabled targets that want a notification with this status: a target with onlyProblems gets none on
    /// success.
    public static func targetsFor(_ targets: [NotifyTarget], status: StatusWord) -> [NotifyTarget] {
        targets.filter { $0.enabled && !($0.onlyProblems && status == .success) }
    }
}
