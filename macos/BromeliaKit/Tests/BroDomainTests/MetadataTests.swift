import BroDomain
import BroFoundation
import BroTestSupport
import Foundation
import Testing

private func kind(_ v: JsonValue?) -> MediaKind { v?.string == "tv" ? .tv : .movie }

private func intOrNil(_ v: JsonValue?) -> Int? { v?.int.map(Int.init) }

/// The request a fixture case describes; nil when the provider can't make it.
private func requestOf(_ given: JsonValue) throws -> HttpRequestSpec? {
    let tmdb = given["provider"]?.string == "tmdb"
    let key = given["key"]?.string ?? ""
    let language = "en-US"
    if let s = given["search"] {
        let name = s["name"]?.string ?? ""
        return tmdb ? TmdbRequests.search(name, kind: kind(s["kind"]), year: intOrNil(s["year"]), key: key, language: language)
            : OmdbRequests.search(name, kind: kind(s["kind"]), year: intOrNil(s["year"]), key: key)
    }
    if let idJson = given["id"] {
        let id: OnlineId = idJson["imdb"]?.string.map { .imdb(id: $0) }
            ?? .tmdb(id: intOrNil(idJson["tmdb"]) ?? 0, kind: idJson["kind"].map { kind($0) })
        if !tmdb { return OmdbRequests.details(id, key: key) }
        switch id {
        case .tmdb(let n, let k): return TmdbRequests.details(n, kind: k ?? kind(given["kind"]), key: key, language: language)
        case .imdb(let tt): return TmdbRequests.find(tt, key: key, language: language)
        }
    }
    if let season = given["season"] {
        let match = Candidate(title: "", year: nil, tmdbId: intOrNil(season["tmdbId"]), imdbId: season["imdbId"]?.string, provider: "",
                              kind: kind(season["kind"]))
        let n = intOrNil(season["season"]) ?? 0
        return tmdb ? TmdbRequests.season(match, season: n, key: key, language: language) : OmdbRequests.season(match, season: n, key: key)
    }
    if let groups = given["episodeGroups"] { return TmdbRequests.episodeGroups(intOrNil(groups["tmdbId"]) ?? 0, key: key, language: language) }
    if let group = given["episodeGroup"] { return TmdbRequests.episodeGroup(group["id"]?.string ?? "", key: key, language: language) }
    try Fixtures.check(false, "no request in the case")
    return nil
}

/// A candidate as the fixtures write one: only the fields the expected value names.
private func sameCandidate(_ want: JsonValue?, _ got: Candidate?, _ what: String) throws {
    guard let want, !want.isNull else {
        try Fixtures.check(got == nil, "\(what): expected none")
        return
    }
    guard let got else {
        try Fixtures.check(false, "\(what): none")
        return
    }
    for m in want.members ?? [] {
        let actual: JsonValue
        switch m.key {
        case "title": actual = .string(got.title)
        case "year": actual = got.year.map { .integer(Int64($0)) } ?? .null
        case "tmdbId": actual = got.tmdbId.map { .integer(Int64($0)) } ?? .null
        case "imdbId": actual = got.imdbId.map { .string($0) } ?? .null
        case "kind": actual = got.kind.map { .string($0.rawValue) } ?? .null
        case "choice": actual = .string(Candidate.choice(got))
        case "poster": actual = .string(got.poster)
        case "overview": actual = .string(got.overview)
        default:
            try Fixtures.check(false, "unknown field \(m.key)")
            continue
        }
        try Fixtures.same(m.value, actual, "\(what).\(m.key)")
    }
}

private func search(_ c: JsonValue, _ body: [UInt8]) -> [Candidate] {
    let list = c["provider"]?.string == "tmdb" ? TmdbParse.candidates(body, kind: kind(c["kind"]))
        : OmdbParse.candidates(body, kind: c["kind"].map { kind($0) })
    return Ranking.rank(list, name: c["name"]?.string ?? "", year: intOrNil(c["year"]))
}

private func recordedAnswers(_ f: JsonValue) throws {
    for c in f["search"]?.array ?? [] {
        let file = c["file"]?.string ?? ""
        let got = search(c, try Fixtures.bytes("lookup/" + file))
        let want = c["expect"]?.array ?? []
        try Fixtures.check(want.count == got.count, "\(file): \(got.count) results")
        for (i, w) in want.enumerated() where i < got.count { try sameCandidate(w, got[i], "\(file)[\(i)]") }
    }
    for c in f["details"]?.array ?? [] {
        let file = c["file"]?.string ?? ""
        let bytes = try Fixtures.bytes("lookup/" + file)
        let got = c["provider"]?.string == "tmdb" ? TmdbParse.details(bytes, kind: kind(c["kind"])) : OmdbParse.details(bytes, kind: kind(c["kind"]))
        try sameCandidate(c["expect"], got, file)
    }
    for c in f["season"]?.array ?? [] {
        let bytes = try Fixtures.bytes("lookup/" + (c["file"]?.string ?? ""))
        let got = c["provider"]?.string == "tmdb" ? TmdbParse.season(bytes) : OmdbParse.season(bytes)
        try Fixtures.check((c["expect"]?.members ?? []).count == got.count, "episode count")
        for m in c["expect"]?.members ?? [] { try Fixtures.same(m.value.string, got[Int(m.key)!]?.title, "episode \(m.key)") }
        for m in c["aired"]?.members ?? [] { try Fixtures.same(m.value.string, got[Int(m.key)!]?.aired, "aired \(m.key)") }
    }
    for c in f["ids"]?.array ?? [] {
        let id = OnlineId.parse(c["text"]?.string ?? "")
        if c["invalid"]?.bool == true {
            try Fixtures.check(id == nil, "\(c) is no id")
        } else if let imdb = c["imdb"]?.string {
            try Fixtures.check(id == .imdb(id: imdb), "\(c)")
        } else {
            try Fixtures.check(id == .tmdb(id: intOrNil(c["tmdb"]) ?? 0, kind: c["kind"].map { kind($0) }), "\(c)")
        }
    }
    for c in f["splitYear"]?.array ?? [] {
        try Fixtures.check(OnlineId.splitYear(c["text"]?.string ?? "") == NameAndYear(name: c["name"]?.string ?? "", year: intOrNil(c["year"])), "\(c)")
    }
}

struct MetadataTests {
    @Test func metadataCases() throws {
        let failures = try Fixtures.runCases("domain/metadata.cases.json") { _, given, expect in
            if let fixture = given["fixture"]?.string {
                try recordedAnswers(try Fixtures.json(fixture))
                return true
            }
            if let body = given["body"]?.string {
                try sameCandidate(expect["best"], search(given, Array(body.utf8)).first, "best")
                return true
            }
            if let file = given["file"]?.string {
                let bytes = try Fixtures.bytes(file)
                if let groupId = expect["groupId"] { try Fixtures.same(groupId.string, TmdbParse.absoluteGroup(bytes), "groupId") }
                if let titles = expect["titles"] {
                    let episodes = TmdbParse.absoluteEpisodes(bytes)
                    for m in titles.members ?? [] { try Fixtures.same(m.value.string, episodes[Int(m.key)!]?.title, "title \(m.key)") }
                }
                return true
            }
            let request = try requestOf(given)
            if (expect.members ?? []).contains(where: { $0.key == "request" }), expect["request"]?.isNull == true {
                try Fixtures.check(request == nil, "no request expected")
                return true
            }
            guard let request, let url = URLComponents(string: request.url) else {
                try Fixtures.check(false, "no request")
                return true
            }
            let query = url.percentEncodedQuery ?? ""
            if let method = expect["method"]?.string { try Fixtures.same(method, request.method, "method") }
            if let path = expect["path"]?.string { try Fixtures.same(path, url.path, "path") }
            for q in expect["queryContains"]?.array ?? [] { try Fixtures.check(query.contains(q.string ?? ""), "query lacks \(q)") }
            for q in expect["queryExcludes"]?.array ?? [] { try Fixtures.check(!query.contains(q.string ?? ""), "query has \(q)") }
            if let none = expect["noHeader"]?.string { try Fixtures.check(!request.headers.contains { $0.name == none }, "header \(none)") }
            for h in expect["header"]?.members ?? [] {
                try Fixtures.check(request.headers.contains { $0.name == h.key && $0.value == h.value.string }, "header \(h.key)")
            }
            return true
        }
        #expect(failures.isEmpty, "\(failures.joined(separator: "\n"))")
    }
}
