/// A UTC time with milliseconds, held as milliseconds since 1970-01-01T00:00:00Z.
public struct Instant: Sendable, Hashable, Comparable, CustomStringConvertible {
    public var unixMilliseconds: Int64

    public init(unixMilliseconds: Int64) { self.unixMilliseconds = unixMilliseconds }

    public static func < (a: Instant, b: Instant) -> Bool { a.unixMilliseconds < b.unixMilliseconds }

    /// RFC 3339: `YYYY-MM-DDTHH:MM:SS`, optional fractional seconds (cut to milliseconds), then `Z` or
    /// `±HH:MM`. Nil for anything else. Written by hand, as on the other platforms, so all three accept exactly
    /// the same texts.
    public static func parse(_ text: String) -> Instant? {
        let b = Array(text.utf8)
        var i = 0
        func number(_ digits: Int) -> Int? {
            guard i + digits <= b.count else { return nil }
            var v = 0
            for k in 0..<digits {
                let c = b[i + k]
                guard c >= 0x30, c <= 0x39 else { return nil }
                v = v * 10 + Int(c - 0x30)
            }
            i += digits
            return v
        }
        func literal(_ c: Character) -> Bool {
            guard i < b.count, b[i] == c.asciiValue! else { return false }
            i += 1
            return true
        }
        guard let year = number(4), literal("-"),
              let month = number(2), (1...12).contains(month), literal("-"),
              let day = number(2), day >= 1, day <= daysInMonth(year, month), literal("T"),
              let hour = number(2), hour <= 23, literal(":"),
              let minute = number(2), minute <= 59, literal(":"),
              let second = number(2), second <= 59 else { return nil }
        var millis = 0
        if literal(".") {
            var digits = 0
            while i < b.count, b[i] >= 0x30, b[i] <= 0x39 {
                if digits < 3 { millis = millis * 10 + Int(b[i] - 0x30) }
                digits += 1
                i += 1
            }
            guard digits > 0 else { return nil }
            for _ in digits..<max(digits, 3) { millis *= 10 }
        }
        var offsetMinutes = 0
        if !literal("Z") {
            let sign = literal("+") ? 1 : literal("-") ? -1 : 0
            guard sign != 0, let oh = number(2), oh <= 23, literal(":"), let om = number(2), om <= 59 else { return nil }
            offsetMinutes = sign * (oh * 60 + om)
        }
        guard i == b.count else { return nil }
        let days = daysFromCivil(Int64(year), Int64(month), Int64(day))
        let seconds = days * 86400 + Int64(hour * 3600 + minute * 60 + second) - Int64(offsetMinutes * 60)
        return Instant(unixMilliseconds: seconds * 1000 + Int64(millis))
    }

    /// `2026-10-03T10:10:28.608Z`.
    public static func format(_ instant: Instant) -> String {
        let ms = instant.unixMilliseconds
        let days = floorDiv(ms, 86_400_000)
        let inDay = ms - days * 86_400_000
        let (y, m, d) = civilFromDays(days)
        func pad(_ n: Int64, _ width: Int) -> String {
            let s = String(n)
            return String(repeating: "0", count: max(0, width - s.count)) + s
        }
        return "\(pad(y, 4))-\(pad(m, 2))-\(pad(d, 2))T\(pad(inDay / 3_600_000, 2)):\(pad(inDay / 60_000 % 60, 2)):"
            + "\(pad(inDay / 1000 % 60, 2)).\(pad(inDay % 1000, 3))Z"
    }

    public var description: String { Instant.format(self) }

    private static func floorDiv(_ a: Int64, _ b: Int64) -> Int64 { a / b - (a % b != 0 && (a < 0) != (b < 0) ? 1 : 0) }

    private static func daysInMonth(_ y: Int, _ m: Int) -> Int {
        if m == 2 { return y % 4 == 0 && (y % 100 != 0 || y % 400 == 0) ? 29 : 28 }
        return [4, 6, 9, 11].contains(m) ? 30 : 31
    }

    // Howard Hinnant's days_from_civil / civil_from_days.
    private static func daysFromCivil(_ year: Int64, _ m: Int64, _ d: Int64) -> Int64 {
        let y = year - (m <= 2 ? 1 : 0)
        let era = floorDiv(y, 400)
        let yoe = y - era * 400
        let doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1
        let doe = yoe * 365 + yoe / 4 - yoe / 100 + doy
        return era * 146097 + doe - 719468
    }

    private static func civilFromDays(_ days: Int64) -> (Int64, Int64, Int64) {
        let z = days + 719468
        let era = floorDiv(z, 146097)
        let doe = z - era * 146097
        let yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365
        let doy = doe - (365 * yoe + yoe / 4 - yoe / 100)
        let mp = (5 * doy + 2) / 153
        let d = doy - (153 * mp + 2) / 5 + 1
        let m = mp + (mp < 10 ? 3 : -9)
        return (yoe + era * 400 + (m <= 2 ? 1 : 0), m, d)
    }
}
