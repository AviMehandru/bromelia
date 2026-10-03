import BroFoundation
import Foundation

/// `mkvextract chapters --simple` output.
public enum SimpleChapters {
    /// The chapter starts, in order: `CHAPTER02=00:23:36.815` → 1416.815 s.
    public static func parse(_ text: String) -> [Duration] {
        let re = try! NSRegularExpression(pattern: #"^CHAPTER\d+=(\d+):(\d+):([\d.]+)"#, options: [.anchorsMatchLines])
        return re.matches(in: text, range: NSRange(text.startIndex..., in: text)).map { m in
            func group(_ i: Int) -> String { String(text[Range(m.range(at: i), in: text)!]) }
            return Duration(seconds: Double(Int(group(1))! * 3600 + Int(group(2))! * 60) + (Double(group(3)) ?? 0))
        }
    }
}
