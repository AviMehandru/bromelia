import BroDomain
import BroFoundation
import BroTestSupport
import Testing

/// A title written in a fixture: any of the Title fields, the rest empty.
func titleOf(_ t: JsonValue) -> Title {
    let duration = t["duration"]?.string ?? ""
    let seconds = t["durationSeconds"]?.int.map(Int.init) ?? Int(Duration.parseClock(duration)?.seconds ?? 0)
    return Title(index: Int(t["index"]?.int ?? 0), sourceTitleId: t["sourceTitleId"]?.int.map(Int.init), sourceFile: t["sourceFile"]?.string ?? "",
                 name: t["name"]?.string ?? "", comment: t["comment"]?.string ?? "", duration: duration, durationSeconds: seconds,
                 chapters: Int(t["chapters"]?.int ?? 0), sizeBytes: t["sizeBytes"]?.int ?? 0, segmentMap: t["segmentMap"]?.string ?? "",
                 outputFileName: t["outputFileName"]?.string ?? "", angle: t["angle"]?.int.map(Int.init), tracks: [], attributes: [:])
}

func listingOfTitles(_ titles: JsonValue?) -> Listing {
    var l = Listing.empty
    l.titles = (titles?.array ?? []).map(titleOf)
    return l
}

func settingsOf(_ r: JsonValue?) -> TitleSettings {
    var s = TitleSettings()
    if let v = r?["strategy"]?.string, let x = TitleStrategy(rawValue: v) { s.strategy = x }
    if let v = r?["longestCount"]?.int { s.longestCount = Int(v) }
    if let v = r?["indexPattern"]?.string { s.indexPattern = v }
    if let v = r?["indexBase"]?.string, let x = IndexBase(rawValue: v) { s.indexBase = x }
    if let v = r?["minDurationSeconds"]?.int { s.minDurationSeconds = Int(v) }
    if let v = r?["maxDurationSeconds"]?.int { s.maxDurationSeconds = Int(v) }
    if let v = r?["minChapters"]?.int { s.minChapters = Int(v) }
    if let v = r?["maxChapters"]?.int { s.maxChapters = Int(v) }
    if let v = r?["minSizeMB"]?.int { s.minSizeMB = Int(v) }
    if let v = r?["maxSizeMB"]?.int { s.maxSizeMB = Int(v) }
    if let v = r?["includePattern"]?.string { s.includePattern = v }
    if let v = r?["excludePattern"]?.string { s.excludePattern = v }
    if let v = r?["skipDuplicates"]?.bool { s.skipDuplicates = v }
    if let v = r?["skipAlternateAngles"]?.bool { s.skipAlternateAngles = v }
    if let v = r?["maxTitles"]?.int { s.maxTitles = Int(v) }
    return s
}

private func ints(_ v: JsonValue?) -> [Int] { (v?.array ?? []).compactMap { $0.int.map(Int.init) } }

struct TitlesTests {
    @Test func titleRulesCases() throws {
        let inputs = try Fixtures.json("domain/titles.cases.json")["inputs"]
        let failures = try Fixtures.runCases("domain/titles.cases.json") { _, given, expect in
            let listing: Listing
            if let name = given["titles"]?.string {
                listing = listingOfTitles(inputs?[name])
            } else {
                listing = listingOf(try Fixtures.text(given["listing"]?.string ?? ""))
            }
            let s = TitleRules.select(listing, rules: settingsOf(given["rules"]))
            try Fixtures.same(expect["selected"], .array(s.indices.map { .integer(Int64($0)) }), "selected")
            try Fixtures.same(expect["requiresManualChoice"]?.bool ?? false, s.requiresManualChoice, "requires manual choice")
            try Fixtures.same(expect["error"]?["code"]?.string, s.error.map { MessageCode.wire($0.code) }, "error")
            for (key, reason) in expect["reasons"]?.members ?? [] {
                try Fixtures.same(reason, s.trace.first { String($0.index) == key }?.reason.toJson(), "reason of title \(key)")
            }
            try Fixtures.same(listing.titles.count, s.trace.count, "trace")
            return true
        }
        #expect(failures.isEmpty, "\(failures.joined(separator: "\n"))")
    }

    @Test func onePassAndSpaceCases() throws {
        let failures = try Fixtures.runCases("domain/one-pass-and-space.cases.json") { _, given, expect in
            if let titles = given["titles"], let chosen = given["chosen"] {
                let plan = OnePass.plan(ints(chosen), listing: listingOfTitles(titles), currentMinLength: given["current"]?.int.map(Int.init))
                try Fixtures.same(expect["minLength"]?.int, plan.map { Int64($0.minLength) }, "min length")
            } else if let listing = given["listing"] {
                try Fixtures.same(expect["matches"]?.bool, OnePass.matches(listingOfTitles(listing), chosen: ints(given["chosen"]), of: listingOfTitles(given["of"])), "matches")
            } else if let bytes = given["bytes"]?.int {
                try Fixtures.same(expect["required"]?.int, SpaceEstimate.required(Bytes(bytes)).count, "required")
            } else if let titles = given["titles"] {
                let need = SpaceEstimate.forPlan(listingOfTitles(titles).titles, handPicked: Set(ints(given["handPicked"])),
                                                 splitTitle: given["splitTitle"]?.int.map(Int.init))
                try Fixtures.same(expect["bytes"]?.int, need.count, "bytes")
                try Fixtures.same(expect["required"]?.int, SpaceEstimate.required(need).count, "required")
            } else {
                return false
            }
            return true
        }
        #expect(failures.isEmpty, "\(failures.joined(separator: "\n"))")
    }

    @Test func listingMatchFindsMovedTitlesAndChangedDiscs() throws {
        func l(_ volume: String, _ ts: [(Int, Int, Int, String)]) -> Listing {
            var x = Listing.empty
            x.volumeName = volume
            x.titles = ts.map { titleOf(.object([("index", .integer(Int64($0.0))), ("sourceTitleId", .integer(Int64($0.1))),
                                                  ("durationSeconds", .integer(Int64($0.2))), ("segmentMap", .string($0.3))])) }
            return x
        }
        let opened = l("SHOW", [(0, 1, 1300, "1"), (1, 2, 1310, "2"), (2, 3, 1320, "3")])
        let renumbered = l("SHOW", [(0, 2, 1310, "2"), (1, 3, 1320, "3")])
        #expect(ListingMatch.differentDisc(opened, renumbered) == nil)
        #expect(try ListingMatch.mapTitles([1, 2], from: opened, to: renumbered, sameTracks: []) == [1: 0, 2: 1])
        #expect(throws: BroError.self) { try ListingMatch.mapTitles([0], from: opened, to: renumbered, sameTracks: []) }
        do {
            _ = try ListingMatch.mapTitles([0], from: opened, to: renumbered, sameTracks: [])
        } catch {
            #expect(error.code == "disc.titleGone")
        }
        #expect(ListingMatch.differentDisc(opened, l("OTHER", [(0, 1, 1300, "1")]))?.code == .discChangedVolume)
        #expect(ListingMatch.differentDisc(opened, l("SHOW", [(0, 9, 99, "9")]))?.code == .discChangedTitles)
    }
}
