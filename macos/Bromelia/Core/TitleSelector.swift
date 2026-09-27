import Foundation

/// Parses index patterns such as `0,2-4,7-`, `last`, `all` or `*`.
struct IndexPattern: Equatable, Sendable {
    enum Item: Equatable, Sendable {
        case single(Int)
        case range(Int, Int?)   // inclusive; nil upper bound = open ended
        case last
        case all
    }

    var items: [Item]

    enum ParseError: Error, LocalizedError, Equatable {
        case invalid(String)
        var errorDescription: String? {
            switch self { case .invalid(let s): return "Invalid index pattern element “\(s)”" }
        }
    }

    init(items: [Item]) { self.items = items }

    init(parsing text: String) throws {
        var result: [Item] = []
        for raw in text.split(whereSeparator: { $0 == "," || $0 == ";" || $0 == " " }) {
            let part = raw.trimmingCharacters(in: .whitespaces).lowercased()
            if part.isEmpty { continue }
            if part == "*" || part == "all" { result.append(.all); continue }
            if part == "last" { result.append(.last); continue }
            if let dash = part.firstIndex(of: "-") {
                let lo = String(part[..<dash]), hi = String(part[part.index(after: dash)...])
                guard let l = Int(lo) else { throw ParseError.invalid(part) }
                if hi.isEmpty { result.append(.range(l, nil)); continue }
                guard let h = Int(hi), h >= l else { throw ParseError.invalid(part) }
                result.append(.range(l, h))
            } else {
                guard let n = Int(part), n >= 0 else { throw ParseError.invalid(part) }
                result.append(.single(n))
            }
        }
        items = result
    }

    func matches(_ value: Int, maxValue: Int) -> Bool {
        items.contains { item in
            switch item {
            case .all: return true
            case .last: return value == maxValue
            case .single(let n): return n == value
            case .range(let l, let h): return value >= l && (h == nil || value <= h!)
            }
        }
    }
}

/// Decision about a single title, with a human readable reason (shown in the rule preview).
struct TitleDecision: Identifiable, Hashable, Sendable {
    var titleIndex: Int
    var selected: Bool
    var reason: String
    var id: Int { titleIndex }
}

enum TitleSelector {
    struct Result: Sendable {
        var decisions: [TitleDecision]
        var selectedIndices: [Int] { decisions.filter(\.selected).map(\.titleIndex).sorted() }
        var requiresManualChoice: Bool
        var error: String?
    }

    static func evaluate(_ titles: [TitleInfo], rule: TitleSelection) -> Result {
        var decisions: [Int: TitleDecision] = [:]
        func reject(_ t: TitleInfo, _ why: String) {
            if decisions[t.index] == nil { decisions[t.index] = TitleDecision(titleIndex: t.index, selected: false, reason: why) }
        }

        var include: NSRegularExpression?
        var exclude: NSRegularExpression?
        var errorText: String?
        if !rule.includePattern.isEmpty {
            include = try? NSRegularExpression(pattern: rule.includePattern, options: [.caseInsensitive])
            if include == nil { errorText = "Invalid include pattern" }
        }
        if !rule.excludePattern.isEmpty {
            exclude = try? NSRegularExpression(pattern: rule.excludePattern, options: [.caseInsensitive])
            if exclude == nil { errorText = "Invalid exclude pattern" }
        }

        // 1. Filters.
        var candidates: [TitleInfo] = []
        for t in titles.sorted(by: { $0.index < $1.index }) {
            let d = t.durationSeconds
            let mb = Int(t.sizeBytes / 1_000_000)
            if rule.minDurationSeconds > 0 && d < rule.minDurationSeconds { reject(t, "Shorter than \(TitleInfo.formatDuration(rule.minDurationSeconds))"); continue }
            if rule.maxDurationSeconds > 0 && d > rule.maxDurationSeconds { reject(t, "Longer than \(TitleInfo.formatDuration(rule.maxDurationSeconds))"); continue }
            if rule.minChapters > 0 && t.chapterCount < rule.minChapters { reject(t, "Fewer than \(rule.minChapters) chapters"); continue }
            if rule.maxChapters > 0 && t.chapterCount > rule.maxChapters { reject(t, "More than \(rule.maxChapters) chapters"); continue }
            if rule.minSizeMB > 0 && mb < rule.minSizeMB { reject(t, "Smaller than \(rule.minSizeMB) MB"); continue }
            if rule.maxSizeMB > 0 && mb > rule.maxSizeMB { reject(t, "Larger than \(rule.maxSizeMB) MB"); continue }
            let haystack = matchText(for: t)
            let range = NSRange(haystack.startIndex..., in: haystack)
            if let inc = include, inc.firstMatch(in: haystack, range: range) == nil { reject(t, "Does not match include pattern"); continue }
            if let exc = exclude, exc.firstMatch(in: haystack, range: range) != nil { reject(t, "Matches exclude pattern"); continue }
            if rule.skipAlternateAngles, let a = t.angle, a > 1 { reject(t, "Alternate angle \(a)"); continue }
            candidates.append(t)
        }

        // 2. Duplicates (same segment map and duration → same content).
        if rule.skipDuplicates {
            var seen: [String: Int] = [:]
            candidates = candidates.filter { t in
                guard !t.segmentMap.isEmpty else { return true }
                let key = "\(t.segmentMap)|\(t.durationSeconds)|\(t.angle ?? 0)"
                if let first = seen[key] { reject(t, "Duplicate of title \(first)"); return false }
                seen[key] = t.index
                return true
            }
        }

        // 3. Strategy.
        var chosen: [TitleInfo] = []
        var manual = false
        switch rule.strategy {
        case .all:
            chosen = candidates
        case .longest:
            let n = max(1, rule.longestCount)
            let ranked = candidates.sorted {
                if $0.durationSeconds != $1.durationSeconds { return $0.durationSeconds > $1.durationSeconds }
                if $0.chapterCount != $1.chapterCount { return $0.chapterCount > $1.chapterCount }
                if $0.sizeBytes != $1.sizeBytes { return $0.sizeBytes > $1.sizeBytes }
                return $0.index < $1.index
            }
            chosen = Array(ranked.prefix(n))
            for t in ranked.dropFirst(n) { reject(t, "Not among the \(n) longest") }
        case .indices:
            do {
                let pattern = try IndexPattern(parsing: rule.indexPattern)
                let maxValue: Int
                switch rule.indexBase {
                case .makemkv: maxValue = titles.map(\.index).max() ?? 0
                case .source: maxValue = titles.compactMap(\.sourceTitleId).max() ?? 0
                }
                for t in candidates {
                    let v = rule.indexBase == .makemkv ? t.index : (t.sourceTitleId ?? -1)
                    if pattern.matches(v, maxValue: maxValue) { chosen.append(t) } else { reject(t, "Not in index pattern") }
                }
            } catch {
                errorText = error.localizedDescription
                for t in candidates { reject(t, "Invalid index pattern") }
            }
        case .manual:
            manual = true
            for t in candidates { decisions[t.index] = TitleDecision(titleIndex: t.index, selected: false, reason: "Choose manually") }
        }

        chosen.sort { $0.index < $1.index }
        if rule.maxTitles > 0 && chosen.count > rule.maxTitles {
            for t in chosen.dropFirst(rule.maxTitles) { reject(t, "Over the limit of \(rule.maxTitles) titles") }
            chosen = Array(chosen.prefix(rule.maxTitles))
        }
        for t in chosen { decisions[t.index] = TitleDecision(titleIndex: t.index, selected: true, reason: "Selected") }

        let ordered = titles.map { decisions[$0.index] ?? TitleDecision(titleIndex: $0.index, selected: false, reason: "Excluded") }
        return Result(decisions: ordered, requiresManualChoice: manual, error: errorText)
    }

    /// Text the include / exclude patterns are matched against.
    static func matchText(for t: TitleInfo) -> String {
        [t.name, t.comment, t.outputFileName, t.sourceFileName, t.segmentMap,
         t.sourceTitleId.map { "#\($0)" } ?? "", t.durationText]
            .filter { !$0.isEmpty }.joined(separator: " ")
    }
}
