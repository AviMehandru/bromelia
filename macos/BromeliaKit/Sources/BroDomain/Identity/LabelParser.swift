import Foundation

/// Reads disc labels.
public enum LabelParser {
    static let noise: Set<String> = ["WS", "FS", "16X9", "4X3", "NTSC", "PAL", "R1", "R2", "R4", "UHD", "4K", "BD", "BLURAY", "BLU", "RAY",
                                     "DVD", "DVD5", "DVD9", "BD25", "BD50", "BD66", "BD100", "HDR", "SDR", "HD", "DISC", "DISK"]
    static let smallWords: Set<String> = ["a", "an", "and", "as", "at", "but", "by", "for", "from", "in", "into", "nor", "of", "on", "or",
                                          "the", "to", "vs", "with"]

    static let seasonDisc = try! NSRegularExpression(pattern: "^S([0-9]{1,2})(?:D([0-9]{1,2}))?(?:E([0-9]{1,3}))?$")
    static let season = try! NSRegularExpression(pattern: "^SEASON([0-9]{1,2})?$")
    static let episode = try! NSRegularExpression(pattern: "^(?:EP|EPS|EPISODE|EPISODES)([0-9]{1,3})?$")
    static let disc = try! NSRegularExpression(pattern: "^(?:D|DISC|DISK|CD)([0-9]{1,2})$")
    static let part = try! NSRegularExpression(pattern: "^(?:P|PT|PART)([0-9]{1,2})$")
    static let volume = try! NSRegularExpression(pattern: "^(?:V|VOL|VOLUME)([0-9]{1,2})$")

    /// Splits a label into words, reads set markers (S2, SEASON 2, P7, VOL 3, D2, DISC 2, S1D2 …) and returns the
    /// cleaned title. Everything from the first set marker on is left out of the title.
    public static func parse(_ label: String) -> Label {
        var l = Label(title: "")
        let cleaned = label.replacingOccurrences(of: "™", with: "").replacingOccurrences(of: "®", with: "")
            .replacingOccurrences(of: "©", with: "").replacingOccurrences(of: " - ", with: " ")
        let separators: Set<Character> = ["_", ".", " ", "\t", "(", ")", "[", "]", ","]
        let tokens = cleaned.split(whereSeparator: { separators.contains($0) }).map(String.init).filter { $0 != "-" }
        var titleTokens: [String] = []
        var stopped = false
        func numberAfter(_ i: Int) -> Int? {
            guard i + 1 < tokens.count, !tokens[i + 1].isEmpty, tokens[i + 1].allSatisfy({ $0.isASCII && $0.isNumber }) else { return nil }
            return Int(tokens[i + 1])
        }
        func match(_ re: NSRegularExpression, _ s: String) -> [String?]? {
            guard let m = re.firstMatch(in: s, range: NSRange(s.startIndex..., in: s)) else { return nil }
            return (1..<m.numberOfRanges).map { g in Range(m.range(at: g), in: s).map { String(s[$0]) } }
        }
        var i = 0
        while i < tokens.count {
            let t = tokens[i]
            let u = asciiUpper(t)
            var marker = true, consumed = false
            if let g = match(seasonDisc, u) {
                l.season = Int(g[0]!)
                if let d = g[1] { l.disc = Int(d) }
                l.looksLikeSeries = true
            } else if let g = match(season, u) {
                if let s = g[0] { l.season = Int(s) } else if let n = numberAfter(i) { l.season = n; consumed = true }
                l.looksLikeSeries = true
            } else if let g = match(episode, u) {
                if g[0] == nil && numberAfter(i) != nil { consumed = true }
                l.looksLikeSeries = true
            } else if let g = match(disc, u) {
                l.disc = Int(g[0]!)
            } else if ["DISC", "DISK", "D"].contains(u), let n = numberAfter(i) {
                l.disc = n; consumed = true
            } else if let g = match(part, u) {
                l.part = Int(g[0]!)
            } else if ["PART", "PT"].contains(u), let n = numberAfter(i) {
                l.part = n; consumed = true
            } else if let g = match(volume, u) {
                l.volume = Int(g[0]!); l.looksLikeSeries = true
            } else if ["VOL", "VOLUME"].contains(u), let n = numberAfter(i) {
                l.volume = n; consumed = true; l.looksLikeSeries = true
            } else {
                marker = false
            }
            if marker { stopped = true } else if !stopped && !noise.contains(u) { titleTokens.append(t) }
            i += consumed ? 2 : 1
        }
        l.title = titleCase(titleTokens)
        return l
    }

    /// "Season 2 Part 7 Disc 2", or "" when the label has no set information. Part of archive names, so never
    /// translated.
    public static func setDescription(_ label: Label) -> String {
        var parts: [String] = []
        if let s = label.season { parts.append("Season \(s)") }
        if let p = label.part { parts.append("Part \(p)") }
        if let v = label.volume { parts.append("Volume \(v)") }
        if let d = label.disc { parts.append("Disc \(d)") }
        return parts.joined(separator: " ")
    }

    /// Title-cases words when the label is all upper or all lower case; mixed case is kept.
    static func titleCase(_ words: [String]) -> String {
        let shouting = words.allSatisfy { $0 == $0.uppercased() } || words.allSatisfy { $0 == $0.lowercased() }
        if !shouting { return words.joined(separator: " ") }
        var out: [String] = []
        for (i, w) in words.enumerated() {
            let lower = w.lowercased()
            if i > 0 && smallWords.contains(lower) { out.append(lower); continue }
            if w.count <= 4 && lower.allSatisfy({ $0 == "i" || $0 == "v" || $0 == "x" }) { out.append(w.uppercased()); continue }
            out.append(lower.split(separator: "-", omittingEmptySubsequences: false)
                .map { $0.isEmpty ? "" : $0.prefix(1).uppercased() + $0.dropFirst() }.joined(separator: "-"))
        }
        return out.joined(separator: " ")
    }

    static func asciiUpper(_ s: String) -> String {
        String(String.UnicodeScalarView(s.unicodeScalars.map { $0.value >= 97 && $0.value <= 122 ? Unicode.Scalar($0.value - 32)! : $0 }))
    }
}
