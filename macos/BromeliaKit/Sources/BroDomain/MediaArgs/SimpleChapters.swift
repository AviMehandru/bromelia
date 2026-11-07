import BroFoundation
import Foundation

/// `mkvextract chapters --simple` output.
public enum SimpleChapters {
    /// The chapter starts, in order: `CHAPTER02=00:23:36.815` → 1416.815 s. A tool wrote this: a line whose time can't be
    /// read is left out.
    public static func parse(_ text: String) -> [Duration] {
        let re = try! NSRegularExpression(pattern: #"^CHAPTER\d+=(\d+):(\d+):([\d.]+)"#, options: [.anchorsMatchLines])
        return re.matches(in: text, range: NSRange(text.startIndex..., in: text)).compactMap { m in
            func group(_ i: Int) -> String { String(text[Range(m.range(at: i), in: text)!]) }
            guard let hours = Int32(group(1)), let minutes = Int32(group(2)), let seconds = Double(group(3)), seconds.isFinite else { return nil }
            return Duration(seconds: Double(hours) * 3600 + Double(minutes) * 60 + seconds)
        }
    }
}
