import Foundation

/// Episode numbers read (by OCR) from a DVD's menus.
public enum MenuNumbers {
    static let episodeText = try! NSRegularExpression(pattern: "EPIS[O0]DE\\s*#?\\s*([0-9]{1,4})\\b", options: .caseInsensitive)

    /// The numbers after "Episode" (OCR may read the O as 0), in the order they appear, each once.
    public static func parse(_ text: String) -> [Int] {
        var out: [Int] = []
        for m in episodeText.matches(in: text, range: NSRange(text.startIndex..., in: text)) {
            if let r = Range(m.range(at: 1), in: text), let n = Int(text[r]), !out.contains(n) { out.append(n) }
        }
        return out
    }

    /// The first episode S such that [S, S+count) covers the most numbers; nil without a unique best choice backed by
    /// at least two numbers.
    public static func firstEpisode(_ numbers: [Int], count: Int) -> Int? {
        let set = Set(numbers)
        if set.isEmpty || count <= 0 { return nil }
        var starts = Set<Int>()
        for n in set { for k in 0..<count where n - k >= 0 { starts.insert(n - k) } }
        let scores = starts.map { s in (s, set.filter { s <= $0 && $0 < s + count }.count) }
        guard let best = scores.map(\.1).max() else { return nil }
        let winners = scores.filter { $0.1 == best }
        return winners.count == 1 && best >= 2 ? winners[0].0 : nil
    }
}
