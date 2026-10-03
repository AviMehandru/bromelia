/// Orders search results.
public enum Ranking {
    /// Lower case, letters and digits only: "Dune: Part Two" → "duneparttwo".
    static func normalize(_ s: String) -> String {
        var out = ""
        for scalar in s.unicodeScalars {
            switch scalar.properties.generalCategory {
            case .uppercaseLetter, .lowercaseLetter, .titlecaseLetter, .modifierLetter, .otherLetter, .decimalNumber:
                out += scalar.properties.lowercaseMapping
            default:
                break
            }
        }
        return out
    }

    /// Best first: the title matching `name` (ignoring case and punctuation) scores 4, the year 2 (1 when it is off by
    /// one); ties keep the provider's order.
    public static func rank(_ candidates: [Candidate], name: String, year: Int?) -> [Candidate] {
        let want = normalize(name)
        func score(_ c: Candidate) -> Int {
            var s = normalize(c.title) == want ? 4 : 0
            if let y = year, let cy = c.year { s += cy == y ? 2 : abs(cy - y) == 1 ? 1 : 0 }
            return s
        }
        let scored: [(index: Int, score: Int, candidate: Candidate)] = candidates.enumerated().map { ($0.offset, score($0.element), $0.element) }
        return scored.sorted { a, b in a.score != b.score ? a.score > b.score : a.index < b.index }.map(\.candidate)
    }
}
