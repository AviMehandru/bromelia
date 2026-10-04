import BroAdapters
import BroDomain
import BroFoundation
import BroPorts
import BroTestSupport
import Foundation
import Testing

/// An HttpClient that answers by URL: the first rule whose match is part of the URL.
final class RoutedHttp: HttpClient, @unchecked Sendable {
    private let lock = NSLock()
    private var sent: [String] = []
    let rules: [JsonValue]
    init(_ rules: [JsonValue]) { self.rules = rules }
    var urls: [String] { lock.withLock { sent } }

    func send(_ request: HttpRequestSpec, cancel: CancellationToken) async throws(BroError) -> HttpResponse {
        lock.withLock { sent.append(request.url) }
        guard let rule = rules.first(where: { request.url.contains($0["match"]!.string!) }) else { throw BroError("test.noAnswer") }
        if let reason = rule["reason"]?.string { throw BroError("http.failed", [("reason", .string(reason))]) }
        let body = rule["file"]?.string.flatMap { try? Fixtures.bytes($0) } ?? Array((rule["text"]?.string ?? "").utf8)
        return HttpResponse(status: Int(rule["status"]?.int ?? 200), headers: [], body: body)
    }
}

/// shared/fixtures/adapters/metadata-client.cases.json.
@Suite(.serialized) struct MetadataClientTests {
    let dir = NSTemporaryDirectory() + "bromelia-metadata-" + UUID().uuidString

    func match(_ choice: String) -> Candidate {
        if case let .tmdb(id, kind)? = OnlineId.parse(choice) { return Candidate(title: "", year: nil, tmdbId: id, imdbId: nil, provider: "TMDb", kind: kind ?? .tv) }
        return Candidate(title: "", year: nil, tmdbId: nil, imdbId: choice, provider: "OMDb", kind: .tv)
    }

    @Test func theSharedCasesPass() async throws {
        try FileManager.default.createDirectory(atPath: dir, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(atPath: dir) }
        let doc = try Fixtures.json("adapters/metadata-client.cases.json")
        var failures: [String] = []
        for (n, c) in (doc["cases"]?.array ?? []).enumerated() {
            do {
                try await run("case\(n)", c["given"]!, c["expect"]!)
            } catch {
                failures.append("\(c["id"]!.string!): \(error)")
            }
        }
        #expect(failures.isEmpty, "\(failures.joined(separator: "\n"))")
    }

    func run(_ name: String, _ given: JsonValue, _ expect: JsonValue) async throws {
        let dbPath = dir + "/" + name + ".sqlite"
        let store = try SqliteStore.open(dbPath, clock: FixedClock())
        defer { store.close() }
        try await store.migrate()
        let http = RoutedHttp(given["answers"]?.array ?? [])
        let client = MetadataClient(http: http, cache: store.lookupCache(), clock: FixedClock(), fs: PlatformFileSystem(), provider: given["provider"]!.string!,
                                    key: "0123456789abcdef0123456789abcdef", language: "en-US")
        var results: [[Candidate]] = []
        var episodes: [[Int: EpisodeDetails]] = []
        let cancel = CancellationToken()
        do {
            for call in given["calls"]?.array ?? [] {
                let c = call.array!
                func s(_ i: Int) -> String { c[i].string! }
                switch s(0) {
                case "search": results.append(try await client.search(s(1), kind: MediaKind(rawValue: s(2))!, year: c[3].int.map { Int($0) }, cancel: cancel))
                case "lookup": results.append([try await client.lookup(OnlineId.parse(s(1))!, kind: MediaKind(rawValue: s(2))!, cancel: cancel)].compactMap { $0 })
                case "season": episodes.append(try await client.season(match(s(1)), season: Int(c[2].int!), cancel: cancel))
                case "absoluteEpisodes": episodes.append(try await client.absoluteEpisodes(match(s(1)), cancel: cancel))
                case "poster": try await client.poster(s(1), destination: dir + "/" + s(2), cancel: cancel)
                default: throw FixtureError("no test handles this call")
                }
            }
            try Fixtures.check(expect["error"] == nil, "expected \(expect["error"]!)")
        } catch let error as BroError {
            guard let want = expect["error"] else { throw error }
            try Fixtures.same(want["code"]?.string, error.code, "code")
            for (k, v) in want["params"]?.members ?? [] { try Fixtures.same(v, JsonValue.object(error.params)[k] ?? .null, k) }
        }
        for (i, list) in (expect["results"]?.array ?? []).enumerated() {
            for (k, w) in (list.array ?? []).enumerated() {
                let got = results[i][k]
                if let t = w["title"]?.string { try Fixtures.same(t, got.title, "title") }
                if let y = w["year"]?.int { try Fixtures.same(Int(y), got.year, "year") }
                if let id = w["tmdbId"]?.int { try Fixtures.same(Int(id), got.tmdbId, "tmdbId") }
                if let o = w["overviewStarts"]?.string { try Fixtures.check(got.overview.hasPrefix(o), "overview \(got.overview)") }
            }
        }
        for (i, e) in (expect["episodes"]?.array ?? []).enumerated() {
            for (k, v) in e.members ?? [] {
                if k == "count" { try Fixtures.same(Int(v.int!), episodes[i].count, "count") } else { try Fixtures.same(v.string, episodes[i][Int(k)!]?.title, k) }
            }
        }
        if let count = expect["requests"]?.int { try Fixtures.same(Int(count), http.urls.count, "requests") }
        if let lacks = expect["lastRequestLacks"]?.string { try Fixtures.check(!(http.urls.last ?? "").contains(lacks), "last request") }
        if let lacks = expect["cacheKeyLacks"]?.string {
            let keys = RawDb(dbPath).rows("SELECT request_key FROM lookup_cache")
            try Fixtures.check(!keys.isEmpty && !(keys[0][0] ?? "").contains(lacks), "cache key \(keys)")
        }
        for (f, text) in expect["file"]?.members ?? [] { try Fixtures.same(text.string, try String(contentsOfFile: dir + "/" + f, encoding: .utf8), f) }
    }
}
