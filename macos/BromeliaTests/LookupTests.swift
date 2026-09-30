import Testing
import Foundation
@testable import Bromelia

/// shared/fixtures/lookup and shared/fixtures/episode-continuation.json: recorded TMDb / OMDb answers and archive
/// records, with the results every platform must get from them.
private let shared = URL(fileURLWithPath: #filePath).deletingLastPathComponent().deletingLastPathComponent()
    .deletingLastPathComponent().appendingPathComponent("shared/fixtures")

private func json(_ url: URL) throws -> [String: Any] {
    try #require(try JSONSerialization.jsonObject(with: Data(contentsOf: url)) as? [String: Any])
}

private func provider(_ s: Any?) -> MetadataProvider { s as? String == "omdb" ? .omdb : .tmdb }
private func kind(_ s: Any?) -> MediaKind { s as? String == "tv" ? .tv : .movie }

/// Checks the keys present in `expect` against `m`.
private func check(_ m: MediaMatch?, _ expect: [String: Any], _ what: String) {
    guard let m else { Issue.record("\(what): no match"); return }
    #expect(m.title == expect["title"] as? String, "\(what)")
    if let y = expect["year"] { #expect(m.year == y as? Int, "\(what): year") }
    if let t = expect["tmdbId"] { #expect(m.tmdbId == t as? Int, "\(what): tmdbId") }
    if let i = expect["imdbId"] { #expect(m.imdbId == i as? String, "\(what): imdbId") }
    if let k = expect["kind"] { #expect(m.kind == kind(k), "\(what): kind") }
    if let c = expect["choice"] { #expect(m.choiceId == c as? String, "\(what): choice") }
    if let p = expect["poster"] { #expect(m.poster == p as? String, "\(what): poster") }
    if let o = expect["overview"] { #expect(m.overview == o as? String, "\(what): overview") }
}

@Suite("Online lookup")
struct LookupTests {
    let dir = shared.appendingPathComponent("lookup")

    @Test func rankedSearchResults() throws {
        let expected = try json(dir.appendingPathComponent("expected.json"))
        for c in expected["search"] as? [[String: Any]] ?? [] {
            let file = c["file"] as! String
            let list = MetadataLookup.candidates(try Data(contentsOf: dir.appendingPathComponent(file)), provider: provider(c["provider"]),
                                                 name: c["name"] as! String, year: c["year"] as? Int, kind: kind(c["kind"]))
            let want = c["expect"] as! [[String: Any]]
            #expect(list.count == want.count, "\(file) \(c["name"]!)")
            for (m, e) in zip(list, want) { check(m, e, "\(file) \(c["name"]!) \(c["year"] ?? "")") }
        }
    }

    @Test func detailsAndSeasons() throws {
        let expected = try json(dir.appendingPathComponent("expected.json"))
        for c in expected["details"] as? [[String: Any]] ?? [] {
            let file = c["file"] as! String
            check(MetadataLookup.details(try Data(contentsOf: dir.appendingPathComponent(file)), provider: provider(c["provider"]), kind: kind(c["kind"])),
                  c["expect"] as! [String: Any], file)
        }
        for c in expected["season"] as? [[String: Any]] ?? [] {
            let file = c["file"] as! String
            let eps = MetadataLookup.season(try Data(contentsOf: dir.appendingPathComponent(file)), provider: provider(c["provider"]))
            let want = c["expect"] as! [String: String]
            #expect(eps.count == want.count, "\(file)")
            for (n, t) in want { #expect(eps[Int(n)!]?.title == t, "\(file) \(n)") }
            for (n, d) in c["aired"] as? [String: String] ?? [:] { #expect(eps[Int(n)!]?.aired == d, "\(file) aired \(n)") }
        }
    }

    @Test func idsAndYears() throws {
        let expected = try json(dir.appendingPathComponent("expected.json"))
        for c in expected["ids"] as? [[String: Any]] ?? [] {
            let text = c["text"] as! String
            let id = OnlineId.parse(text)
            if c["invalid"] as? Bool == true {
                #expect(id == nil, "\(text)")
            } else if let tt = c["imdb"] as? String {
                #expect(id == .imdb(tt), "\(text)")
            } else {
                #expect(id == .tmdb(c["tmdb"] as! Int, c["kind"].map(kind)), "\(text)")
            }
        }
        for c in expected["splitYear"] as? [[String: Any]] ?? [] {
            let r = MetadataLookup.splitYear(c["text"] as! String)
            #expect(r.name == c["name"] as? String && r.year == c["year"] as? Int, "\(c["text"]!)")
        }
    }

    @Test func requests() throws {
        var c = MetadataConfig()
        c.provider = .tmdb
        c.apiKey = "0123456789abcdef0123456789abcdef"
        let search = try #require(MetadataLookup.request(name: "Friends", kind: .tv, year: 1994, config: c))
        #expect(search.url?.path == "/3/search/tv" && search.url?.query?.contains("first_air_date_year=1994") == true)
        #expect(MetadataLookup.request(id: .tmdb(603, nil), kind: .movie, config: c)?.url?.path == "/3/movie/603")
        #expect(MetadataLookup.request(id: .tmdb(1668, .tv), kind: .movie, config: c)?.url?.path == "/3/tv/1668")
        #expect(MetadataLookup.request(id: .imdb("tt0108778"), kind: .tv, config: c)?.url?.query?.contains("external_source=imdb_id") == true)
        let show = MediaMatch(title: "Friends", year: 1994, tmdbId: 1668, imdbId: "tt0108778", provider: "TMDb", kind: .tv)
        #expect(MetadataLookup.seasonRequest(match: show, season: 2, config: c)?.url?.path == "/3/tv/1668/season/2")
        c.provider = .omdb
        #expect(MetadataLookup.request(id: .tmdb(603, nil), kind: .movie, config: c) == nil)
        let s = try #require(MetadataLookup.seasonRequest(match: show, season: 2, config: c))
        #expect(s.url?.query?.contains("i=tt0108778") == true && s.url?.query?.contains("Season=2") == true)
        let o = try #require(MetadataLookup.request(name: "Dune", kind: .movie, year: 1984, config: c))
        #expect(o.url?.query?.contains("s=Dune") == true && o.url?.query?.contains("y=1984") == true)
    }

    @Test func episodeTitlesInNames() {
        var v = TemplateRenderer.dateValues()
        for (k, x) in MediaIdentity.resolve(info: nil, discLabel: "FRIENDS_S2_D1", encrypted: false).templateValues(rip: "Rip") { v[k] = x }
        v["releaseYear"] = "1994"; v["seasonOr1"] = "2"; v["track"] = "Title 1"
        v["episode"] = "Episode 02"; v["episodeNumber"] = "2"; v["episodeTitle"] = "The One with the Breast Milk"
        #expect(TemplateRenderer.renderPath(MediaServerNaming.episodeTemplate, values: v)
                == "Season 02/Friends (1994) - S02E02 - The One with the Breast Milk")
        #expect(TemplateRenderer.renderPath(OutputConfig.defaultFileNameTemplate, values: v)
                == "Friends - Episode 02 - The One with the Breast Milk - Season 2 Disc 1 - Rip - Title 1 - DISC")
        v["episodeTitle"] = ""
        #expect(TemplateRenderer.renderPath(MediaServerNaming.episodeTemplate, values: v) == "Season 02/Friends (1994) - S02E02")
    }
}

@Suite("Episode numbering across discs")
struct EpisodeContinuationTests {
    @Test func sharedCases() throws {
        let fixture = try json(shared.appendingPathComponent("episode-continuation.json"))
        let base = FileManager.default.temporaryDirectory.appendingPathComponent("bromelia-cont-\(UUID().uuidString.prefix(6))")
        defer { try? FileManager.default.removeItem(at: base) }
        for r in fixture["records"] as! [[String: Any]] {
            let url = base.appendingPathComponent(r["path"] as! String)
            try FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
            try JSONSerialization.data(withJSONObject: r["record"]!).write(to: url)
        }
        for f in fixture["files"] as! [String] {
            let url = base.appendingPathComponent(f)
            try FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
            try Data().write(to: url)
        }
        for c in fixture["cases"] as! [[String: Any]] {
            let q = c["query"] as! [String: Any]
            let query = EpisodeContinuation.Query(name: q["name"] as! String, labelTitle: q["labelTitle"] as! String, season: q["season"] as? Int,
                                                  part: q["part"] as? Int, volume: q["volume"] as? Int, disc: q["disc"] as! Int)
            let found = EpisodeContinuation.find(query, root: base.appendingPathComponent("library"),
                                                 folders: (c["folders"] as? [String] ?? []).map { base.appendingPathComponent($0).path },
                                                 seasonFolder: (c["seasonFolder"] as? String).map { base.appendingPathComponent($0) },
                                                 season: c["season"] as? Int ?? 1)
            #expect(found?.lastEpisode == c["expect"] as? Int, "\(c["what"]!)")
        }
    }
}
