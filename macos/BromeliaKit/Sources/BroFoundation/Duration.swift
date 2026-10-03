/// A length of time in seconds.
public struct Duration: Sendable, Hashable, Comparable {
    public var seconds: Double

    public init(seconds: Double) { self.seconds = seconds }

    public static func < (a: Duration, b: Duration) -> Bool { a.seconds < b.seconds }

    /// makemkvcon's `h:mm:ss` (or `m:ss`, or seconds): `2:44:48` → 9888 s. A part that isn't a number counts as
    /// 0, as today; nil when the text is empty.
    public static func parseClock(_ text: String) -> Duration? {
        let isSpace: (Character) -> Bool = { $0 == " " || $0 == "\t" || $0 == "\n" || $0 == "\r" }
        guard text.contains(where: { !isSpace($0) }) else { return nil }
        var total: Int64 = 0
        for part in text.split(separator: ":", omittingEmptySubsequences: false) {
            let trimmed = part.drop(while: isSpace).reversed().drop(while: isSpace).reversed()
            let digits = String(trimmed)
            let n = !digits.isEmpty && digits.allSatisfy { $0.isASCII && $0.isNumber } ? Int64(digits) ?? 0 : 0
            total = total * 60 + n
        }
        return Duration(seconds: Double(total))
    }

    /// `h:mm:ss`, whole seconds: 9888 s → `2:44:48`, 59 s → `0:00:59`.
    public static func formatClock(_ duration: Duration) -> String {
        let s = Int64(max(0, duration.seconds).rounded(.down))
        func two(_ n: Int64) -> String { n < 10 ? "0\(n)" : "\(n)" }
        return "\(s / 3600):\(two(s / 60 % 60)):\(two(s % 60))"
    }
}
