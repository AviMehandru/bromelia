import BroDomain
import BroFoundation
import BroTestSupport
import Testing

/// A listing written in a fixture: a file name, or {attributes, titles} with tracks.
func listingFrom(_ v: JsonValue?) throws -> Listing {
    if let file = v?.string { return listingOf(try Fixtures.text(file)) }
    func attrs(_ a: JsonValue?) -> [Int: String] {
        Dictionary(uniqueKeysWithValues: (a?.members ?? []).map { (Int($0.key)!, $0.value.string ?? "") })
    }
    let a = attrs(v?["attributes"])
    let titles = (v?["titles"]?.array ?? []).map { t in
        var title = titleOf(t)
        title.tracks = (t["tracks"]?.array ?? []).map { k in
            Track(index: Int(k["index"]?.int ?? 0), kind: TrackKind(rawValue: k["kind"]?.string ?? "") ?? .unknown, codec: "", language: "",
                  languageName: "", name: "", isDefault: false, attributes: attrs(k["attributes"]))
        }
        return title
    }
    return Listing(name: a[2] ?? a[32] ?? "", volumeName: a[32] ?? "", type: .disc, typeText: a[1] ?? "", reportedTitleCount: titles.count,
                   titles: titles, attributes: a)
}

private func orNull(_ v: Int?) -> JsonValue { v.map { .integer(Int64($0)) } ?? .null }

private func labelJson(_ l: Label) -> JsonValue {
    .object([("title", .string(l.title)), ("season", orNull(l.season)), ("part", orNull(l.part)), ("volume", orNull(l.volume)),
             ("disc", orNull(l.disc)), ("looksLikeSeries", .bool(l.looksLikeSeries))])
}

private func flagsOf(_ flags: JsonValue) -> DiscFlags {
    var raw = 0
    for f in flags.array ?? [] {
        switch f.string {
        case "dvdFiles": raw |= 1
        case "hdDvdFiles": raw |= 2
        case "blurayFiles": raw |= 4
        case "aacsFiles": raw |= 8
        case "bdsvmFiles": raw |= 16
        default: break
        }
    }
    return DiscFlags(raw: raw)
}

struct IdentityTests {
    @Test func identityCases() throws {
        let failures = try Fixtures.runCases("domain/identity.cases.json", only: { !$0.hasPrefix("drive-") }) { _, given, expect in
            if let label = given["label"]?.string {
                let parsed = LabelParser.parse(label)
                if let n = given["playAllEpisodes"]?.int {
                    let d = KindHeuristics.decide(parsed, listing: nil, playAllEpisodes: Int(n))
                    try Fixtures.same(expect["kind"]?.string, d.value.rawValue, "kind")
                    try Fixtures.same(expect["reason"], d.reason.toJson(), "reason")
                } else if let set = expect["set"] {
                    try Fixtures.same(set.string, LabelParser.setDescription(parsed), "set")
                } else {
                    try Fixtures.same(expect, labelJson(parsed), "label")
                }
                return true
            }
            if let listingJson = given["listing"] {
                let listing = try listingFrom(listingJson)
                if let like = expect["episodeLike"] {
                    try Fixtures.same(like, .array(KindHeuristics.episodeLike(listing.titles).map { .integer(Int64($0.index)) }), "episode-like")
                    let d = KindHeuristics.decide(LabelParser.parse(listing.volumeName), listing: listing, playAllEpisodes: 0)
                    try Fixtures.same(expect["kind"]?.string, d.value.rawValue, "kind")
                    try Fixtures.same(expect["reason"], d.reason.toJson(), "reason")
                } else if given["encrypted"] == nil {
                    try Fixtures.same(expect["format"]?.string, FormatDetector.detect(listing, flags: nil, indexBdmv: nil).rawValue, "format")
                } else {
                    let identity = Identity.resolve(IdentityInputs(listing: listing, discLabel: "", encrypted: given["encrypted"]?.bool ?? false,
                                                                   nameOverride: given["nameOverride"]?.string ?? "",
                                                                   kindOverride: given["kindOverride"]?.string.flatMap(MediaKind.init(rawValue:))))
                    let actual = JsonValue.object([
                        ("name", .string(identity.name)), ("kind", .string(identity.kind.rawValue)), ("format", .string(identity.format.rawValue)),
                        ("formatCode", .string(identity.formatCode.text)), ("set", .string(LabelParser.setDescription(identity.label))),
                        ("reason", identity.reason.toJson()), ("label", labelJson(identity.label)),
                    ])
                    for (key, value) in expect.members ?? [] {
                        if key == "label" {
                            for (k, v) in value.members ?? [] { try Fixtures.same(v, actual["label"]?[k], "label.\(k)") }
                        } else {
                            try Fixtures.same(value, actual[key], key)
                        }
                    }
                }
                return true
            }
            if let format = given["format"]?.string {
                try Fixtures.same(expect["formatCode"]?.string,
                                  FormatDetector.code(DiscFormat(rawValue: format)!, encrypted: given["encrypted"]?.bool ?? false).text, "format code")
                return true
            }
            if let numbers = given["numbers"]?.array {
                let first = MenuNumbers.firstEpisode(numbers.compactMap { $0.int.map(Int.init) }, count: Int(given["count"]?.int ?? 0))
                try Fixtures.same(expect["firstEpisode"]?.int, first.map(Int64.init), "first episode")
                return true
            }
            if let text = given["text"]?.string {
                try Fixtures.same(expect["numbers"], .array(MenuNumbers.parse(text).map { .integer(Int64($0)) }), "numbers")
                return true
            }
            if let format = expect["format"]?.string {
                let detected = FormatDetector.detect(nil, flags: given["flags"].map(flagsOf), indexBdmv: given["indexBdmv"]?.string,
                                                     hasVideoTs: given["backupHasVideoTs"]?.bool ?? false)
                try Fixtures.same(format, detected.rawValue, "format")
                return true
            }
            return false
        }
        #expect(failures.isEmpty, "\(failures.joined(separator: "\n"))")
    }

    /// shared/fixtures/episode-continuation.json: the records a case can see are the library's (the output root) and
    /// the history's folders; the season folder's file names give its highest episode.
    @Test func episodeContinuationCases() throws {
        let doc = try Fixtures.json("episode-continuation.json")
        func folderOf(_ path: String) -> String { String(path[..<path.lastIndex(of: "/")!]) }
        let records: [ArchivedDisc] = (doc["records"]?.array ?? []).compactMap { r in
            guard let rec = r["record"], let disc = rec["disc"], let path = r["path"]?.string,
                  ["success", "errors"].contains(rec["status"]?.string ?? ""), rec["kind"]?.string == "tv" else { return nil }
            let volume = disc["volumeName"]?.string ?? ""
            let label = LabelParser.parse(volume.isEmpty ? disc["label"]?.string ?? "" : volume)
            let last = (rec["episodes"]?.array ?? []).compactMap { $0["episode"]?.int.map(Int.init) }.max()
            return ArchivedDisc(name: rec["name"]?.string ?? "", labelTitle: label.title, season: disc["season"]?.int.map(Int.init),
                                part: disc["part"]?.int.map(Int.init), volume: disc["volume"]?.int.map(Int.init), disc: disc["disc"]?.int.map(Int.init),
                                lastEpisode: last, folder: folderOf(path))
        }
        let files = (doc["files"]?.array ?? []).compactMap(\.string)
        for c in doc["cases"]?.array ?? [] {
            let q = c["query"]
            let folders = Set((c["folders"]?.array ?? []).compactMap(\.string))
            let visible = records.filter { $0.folder.hasPrefix("library/") || folders.contains($0.folder) }
            let seasonFolder = c["seasonFolder"]?.string
            let highest = seasonFolder.flatMap { sf in
                EpisodeContinuation.highestInSeason(files.filter { folderOf($0) == sf }.map { String($0[$0.index(after: $0.lastIndex(of: "/")!)...]) },
                                                    season: Int(c["season"]?.int ?? 0))
            }
            let query = ContinuationQuery(name: q?["name"]?.string ?? "", labelTitle: q?["labelTitle"]?.string ?? "",
                                          season: q?["season"]?.int.map(Int.init), part: q?["part"]?.int.map(Int.init),
                                          volume: q?["volume"]?.int.map(Int.init), disc: Int(q?["disc"]?.int ?? 0),
                                          seasonFolder: seasonFolder, seasonFolderHighest: highest)
            #expect(c["expect"]?.int == EpisodeContinuation.choose(query, candidates: visible).map { Int64($0.lastEpisode) }, "\(c["what"]?.string ?? "")")
        }
    }
}
