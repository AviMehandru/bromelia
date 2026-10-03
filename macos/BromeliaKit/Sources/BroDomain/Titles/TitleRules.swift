import BroFoundation
import Foundation

/// Which titles of a listing to rip (today's TitleSelector).
public enum TitleRules {
    /// Filters, then duplicates (same segment map, length and angle), then the strategy, then maxTitles. Every
    /// title gets a reason.
    public static func select(_ listing: Listing, rules: TitleSettings) -> Selection {
        let titles = listing.titles
        var reasons: [Int: (selected: Bool, reason: BroMessage)] = [:]
        func reject(_ t: Title, _ why: BroMessage) { if reasons[t.index] == nil { reasons[t.index] = (false, why) } }
        func m(_ code: MessageCode, _ params: [(key: String, value: JsonValue)] = []) -> BroMessage { BroMessage(code, params) }
        func clock(_ s: Int) -> JsonValue { .string(Duration.formatClock(Duration(seconds: Double(s)))) }

        var include: NSRegularExpression?, exclude: NSRegularExpression?
        var error: BroMessage?
        if !rules.includePattern.isEmpty {
            include = try? NSRegularExpression(pattern: rules.includePattern, options: .caseInsensitive)
            if include == nil { error = BroMessage(.titlesInvalidInclude, severity: .error) }
        }
        if !rules.excludePattern.isEmpty {
            exclude = try? NSRegularExpression(pattern: rules.excludePattern, options: .caseInsensitive)
            if exclude == nil { error = BroMessage(.titlesInvalidExclude, severity: .error) }
        }
        func matches(_ re: NSRegularExpression, _ text: String) -> Bool {
            re.firstMatch(in: text, range: NSRange(text.startIndex..., in: text)) != nil
        }

        var candidates: [Title] = []
        for t in titles.sorted(by: { $0.index < $1.index }) {
            let d = t.durationSeconds
            let mb = t.sizeBytes / 1_000_000
            if rules.minDurationSeconds > 0 && d < rules.minDurationSeconds { reject(t, m(.titlesReasonShorterThan, [("duration", clock(rules.minDurationSeconds))])); continue }
            if rules.maxDurationSeconds > 0 && d > rules.maxDurationSeconds { reject(t, m(.titlesReasonLongerThan, [("duration", clock(rules.maxDurationSeconds))])); continue }
            if rules.minChapters > 0 && t.chapters < rules.minChapters { reject(t, m(.titlesReasonFewerChapters, [("count", .integer(Int64(rules.minChapters)))])); continue }
            if rules.maxChapters > 0 && t.chapters > rules.maxChapters { reject(t, m(.titlesReasonMoreChapters, [("count", .integer(Int64(rules.maxChapters)))])); continue }
            if rules.minSizeMB > 0 && mb < Int64(rules.minSizeMB) { reject(t, m(.titlesReasonSmallerThan, [("mb", .integer(Int64(rules.minSizeMB)))])); continue }
            if rules.maxSizeMB > 0 && mb > Int64(rules.maxSizeMB) { reject(t, m(.titlesReasonLargerThan, [("mb", .integer(Int64(rules.maxSizeMB)))])); continue }
            let hay = matchText(t)
            if let include, !matches(include, hay) { reject(t, m(.titlesReasonNoIncludeMatch)); continue }
            if let exclude, matches(exclude, hay) { reject(t, m(.titlesReasonExcludeMatch)); continue }
            if rules.skipAlternateAngles, let angle = t.angle, angle > 1 { reject(t, m(.titlesReasonAlternateAngle, [("angle", .integer(Int64(angle)))])); continue }
            candidates.append(t)
        }

        if rules.skipDuplicates {
            var seen: [String: Int] = [:]
            candidates = candidates.filter { t in
                if t.segmentMap.isEmpty { return true }
                let key = "\(t.segmentMap)|\(t.durationSeconds)|\(t.angle ?? 0)"
                if let first = seen[key] { reject(t, m(.titlesReasonDuplicateOf, [("title", .integer(Int64(first)))])); return false }
                seen[key] = t.index
                return true
            }
        }

        var chosen: [Title] = []
        var manual = false
        switch rules.strategy {
        case .all:
            chosen = candidates
        case .longest:
            let n = max(1, rules.longestCount)
            let ranked = candidates.sorted { a, b in
                if a.durationSeconds != b.durationSeconds { return a.durationSeconds > b.durationSeconds }
                if a.chapters != b.chapters { return a.chapters > b.chapters }
                if a.sizeBytes != b.sizeBytes { return a.sizeBytes > b.sizeBytes }
                return a.index < b.index
            }
            chosen = Array(ranked.prefix(n))
            for t in ranked.dropFirst(n) { reject(t, m(.titlesReasonNotLongest, [("count", .integer(Int64(n)))])) }
        case .indices:
            guard let pattern = IndexPattern.parse(rules.indexPattern) else {
                error = BroMessage(.configInvalidIndexPattern, severity: .error)
                for t in candidates { reject(t, m(.titlesReasonInvalidPattern)) }
                break
            }
            let bySource = rules.indexBase == .source
            let maxValue = bySource ? (titles.map { $0.sourceTitleId ?? 0 }.max() ?? 0) : (titles.map(\.index).max() ?? 0)
            for t in candidates {
                let v = bySource ? t.sourceTitleId ?? -1 : t.index
                if pattern.matches(v, maxValue) { chosen.append(t) } else { reject(t, m(.titlesReasonNotInPattern)) }
            }
        case .manual:
            manual = true
            for t in candidates { reasons[t.index] = (false, m(.titlesReasonChooseManually)) }
        }

        chosen.sort { $0.index < $1.index }
        if rules.maxTitles > 0 && chosen.count > rules.maxTitles {
            for t in chosen.dropFirst(rules.maxTitles) { reject(t, m(.titlesReasonOverLimit, [("count", .integer(Int64(rules.maxTitles)))])) }
            chosen = Array(chosen.prefix(rules.maxTitles))
        }
        for t in chosen { reasons[t.index] = (true, m(.titlesReasonSelected)) }

        let trace = titles.map { t in
            reasons[t.index].map { SelectionTrace(index: t.index, selected: $0.selected, reason: $0.reason) }
                ?? SelectionTrace(index: t.index, selected: false, reason: m(.titlesReasonExcluded))
        }
        return Selection(indices: chosen.map(\.index), trace: trace, requiresManualChoice: manual, error: error)
    }

    /// The text include and exclude patterns are matched against: name, comment, output file name, source file
    /// name, segment map, `#<source id>` and duration.
    static func matchText(_ t: Title) -> String {
        [t.name, t.comment, t.outputFileName, t.sourceFile, t.segmentMap, t.sourceTitleId.map { "#\($0)" } ?? "", t.duration]
            .filter { !$0.isEmpty }.joined(separator: " ")
    }

    /// Index patterns such as `0,2-4,7-`, `last`, `all` or `*`.
    private struct IndexPattern {
        var ranges: [(low: Int, high: Int)] = []  // high Int.max = open
        var all = false, last = false

        static func parse(_ text: String) -> IndexPattern? {
            var p = IndexPattern()
            for raw in text.split(whereSeparator: { $0 == "," || $0 == ";" || $0 == " " }) {
                let part = MessageCatalog.asciiLower(DriveJoin.trimSpaces(String(raw)))
                if part == "*" || part == "all" { p.all = true; continue }
                if part == "last" { p.last = true; continue }
                if let dash = part.firstIndex(of: "-") {
                    guard let l = Robot.int(String(part[..<dash])) else { return nil }
                    let hi = String(part[part.index(after: dash)...])
                    if hi.isEmpty { p.ranges.append((l, Int.max)); continue }
                    guard let h = Robot.int(hi), h >= l else { return nil }
                    p.ranges.append((l, h))
                } else {
                    guard let n = Robot.int(part), n >= 0 else { return nil }
                    p.ranges.append((n, n))
                }
            }
            return p
        }

        func matches(_ value: Int, _ maxValue: Int) -> Bool {
            all || (last && value == maxValue) || ranges.contains { value >= $0.low && value <= $0.high }
        }
    }
}
