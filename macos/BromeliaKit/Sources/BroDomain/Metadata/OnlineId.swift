import Foundation

/// A movie or show chosen by its id: a TMDb id (`603`, `tmdb:603`, `movie/603`, `tv/1668`, a themoviedb.org address;
/// no kind when it doesn't say) or an IMDb id (`tt0133093`, an imdb.com address).
public enum OnlineId: Sendable, Equatable {
    case tmdb(id: Int, kind: MediaKind?)
    case imdb(id: String)

    static func firstMatch(_ pattern: String, _ text: String) -> [String?]? {
        let re = try! NSRegularExpression(pattern: pattern)
        guard let m = re.firstMatch(in: text, range: NSRange(text.startIndex..., in: text)) else { return nil }
        return (0..<m.numberOfRanges).map { i in Range(m.range(at: i), in: text).map { String(text[$0]) } }
    }

    /// The id in `text` (case and surrounding space ignored); nil when there is none.
    public static func parse(_ text: String) -> OnlineId? {
        let t = MessageCatalog.asciiLower(text.trimmingCharacters(in: .whitespacesAndNewlines))
        if let m = firstMatch(#"tt\d{5,10}"#, t) { return .imdb(id: m[0]!) }
        func kind(_ g: String?) -> MediaKind? { g.map { $0 == "tv" ? .tv : .movie } }
        if let u = firstMatch(#"themoviedb\.org/(movie|tv)/(\d+)"#, t) {
            guard let n = Int(u[2]!), n > 0 else { return nil }
            return .tmdb(id: n, kind: kind(u[1]))
        }
        if let m = firstMatch(#"^(?:tmdb:)?(?:(movie|tv)/)?(\d{1,9})$"#, t), let n = Int(m[2]!), n > 0 { return .tmdb(id: n, kind: kind(m[1])) }
        return nil
    }

    /// A year typed after the name: "Inception (2010)" → Inception, 2010 (years 1870 to 2100); otherwise the trimmed
    /// text and no year.
    public static func splitYear(_ text: String) -> NameAndYear {
        let t = text.trimmingCharacters(in: .whitespacesAndNewlines)
        if let m = firstMatch(#"^(.*\S)\s*\((\d{4})\)$"#, t), let y = Int(m[2]!), (1870...2100).contains(y) { return NameAndYear(name: m[1]!, year: y) }
        return NameAndYear(name: t, year: nil)
    }
}
