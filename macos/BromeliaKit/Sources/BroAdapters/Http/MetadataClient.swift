import BroDomain
import BroFoundation
import BroPorts
import Foundation

/// Online lookup (plan §10.2; shared/fixtures/adapters/metadata-client.cases.json): Domain's TMDb and OMDb requests and
/// parsers over the HttpClient. A 200 answer is kept in the lookup cache for 7 days, keyed by the provider and the
/// request without the key. metadata.http for another status; http.failed is passed on; metadata.idUnsupported for an
/// id the provider can't take.
public final class MetadataClient: Sendable {
    private static let cacheMilliseconds: Int64 = 7 * 24 * 3600 * 1000
    private let http: any HttpClient
    private let cache: (any LookupCacheRepository)?
    private let clock: any Clock
    private let fs: any FileSystem
    private let omdb: Bool
    private let key: String
    private let language: String

    /// - Parameters:
    ///   - provider: "tmdb" or "omdb" (metadata.provider).
    ///   - cache: none: nothing is cached.
    public init(http: any HttpClient, cache: (any LookupCacheRepository)?, clock: any Clock, fs: any FileSystem, provider: String, key: String,
                language: String) {
        self.http = http
        self.cache = cache
        self.clock = clock
        self.fs = fs
        omdb = provider == "omdb"
        self.key = key
        self.language = language.isEmpty ? "en-US" : language
    }

    private var provider: String { omdb ? "OMDb" : "TMDb" }

    /// Ranked; a year that finds nothing is dropped; OMDb's best result is read again for its plot.
    public func search(_ name: String, kind: MediaKind, year: Int?, cancel: CancellationToken) async throws(BroError) -> [Candidate] {
        func find(_ y: Int?) async throws(BroError) -> [Candidate] {
            omdb ? OmdbParse.candidates(try await fetch(OmdbRequests.search(name, kind: kind, year: y, key: key), cancel), kind: kind)
                : TmdbParse.candidates(try await fetch(TmdbRequests.search(name, kind: kind, year: y, key: key, language: language), cancel), kind: kind)
        }
        var list = Ranking.rank(try await find(year), name: name, year: year)
        if list.isEmpty, year != nil { list = Ranking.rank(try await find(nil), name: name, year: year) }
        if omdb, let tt = list.first?.imdbId, let request = OmdbRequests.details(.imdb(id: tt), key: key),
           let full = OmdbParse.details(try await fetch(request, cancel), kind: kind) {
            list[0] = full
        }
        return list
    }

    public func lookup(_ id: OnlineId, kind: MediaKind, cancel: CancellationToken) async throws(BroError) -> Candidate? {
        if omdb {
            guard let request = OmdbRequests.details(id, key: key) else {
                throw BroMessage(.metadataIdUnsupported, [("provider", .string(provider)), ("id", .string(Self.shown(id))), ("omdb", .bool(true))],
                                 severity: .error).toError()
            }
            return OmdbParse.details(try await fetch(request, cancel), kind: kind)
        }
        switch id {
        case .tmdb(let n, let k):
            return TmdbParse.details(try await fetch(TmdbRequests.details(n, kind: k ?? kind, key: key, language: language), cancel), kind: k ?? kind)
        case .imdb(let tt):
            return TmdbParse.details(try await fetch(TmdbRequests.find(tt, key: key, language: language), cancel), kind: kind)
        }
    }

    public func season(_ match: Candidate, season: Int, cancel: CancellationToken) async throws(BroError) -> [Int: EpisodeDetails] {
        if omdb {
            guard let r = OmdbRequests.season(match, season: season, key: key) else { return [:] }
            return OmdbParse.season(try await fetch(r, cancel))
        }
        guard let r = TmdbRequests.season(match, season: season, key: key, language: language) else { return [:] }
        return TmdbParse.season(try await fetch(r, cancel))
    }

    /// TMDb's absolute episode order (an episode group of type 2); empty when there is none or the provider is OMDb.
    public func absoluteEpisodes(_ match: Candidate, cancel: CancellationToken) async throws(BroError) -> [Int: EpisodeDetails] {
        guard !omdb, let id = match.tmdbId else { return [:] }
        let groups = try await fetch(TmdbRequests.episodeGroups(id, key: key, language: language), cancel)
        guard let group = TmdbParse.absoluteGroup(groups) else { return [:] }
        return TmdbParse.absoluteEpisodes(try await fetch(TmdbRequests.episodeGroup(group, key: key, language: language), cancel))
    }

    /// Downloads a poster to destination (written atomically, not cached).
    public func poster(_ url: String, destination: String, cancel: CancellationToken) async throws(BroError) {
        let response = try await http.send(HttpRequestSpec(method: "GET", url: url), cancel: cancel)
        guard response.status == 200 else { throw httpError(response.status) }
        try fs.writeAtomically(destination, bytes: response.body, mode: 0o644)
    }

    private func fetch(_ request: HttpRequestSpec, _ cancel: CancellationToken) async throws(BroError) -> [UInt8] {
        let providerKey = omdb ? "omdb" : "tmdb"
        let cacheKey = request.method + " " + Self.withoutKey(request.url)
        if let cache, let cached = try await cache.get(providerKey, key: cacheKey) { return cached.body }
        let response = try await http.send(request, cancel: cancel)
        guard response.status == 200 else { throw httpError(response.status) }
        if let cache {
            try await cache.put(providerKey, key: cacheKey, response: response,
                                expiresAt: Instant(unixMilliseconds: clock.now().unixMilliseconds + Self.cacheMilliseconds))
        }
        return response.body
    }

    private func httpError(_ status: Int) -> BroError {
        BroMessage(.metadataHttp, [("provider", .string(provider)), ("status", .integer(Int64(status)))], severity: .error).toError()
    }

    /// The URL without its api_key / apikey parameter.
    static func withoutKey(_ url: String) -> String {
        guard let q = url.firstIndex(of: "?") else { return url }
        let kept = url[url.index(after: q)...].split(separator: "&", omittingEmptySubsequences: false)
            .filter { !$0.hasPrefix("api_key=") && !$0.hasPrefix("apikey=") }
        return String(url[..<q]) + (kept.isEmpty ? "" : "?" + kept.joined(separator: "&"))
    }

    /// An id as the user writes it: movie/603, tv/1668, tt0133093.
    private static func shown(_ id: OnlineId) -> String {
        switch id {
        case .tmdb(let n, .movie?): return "movie/\(n)"
        case .tmdb(let n, .tv?): return "tv/\(n)"
        case .tmdb(let n, nil): return "\(n)"
        case .imdb(let tt): return tt
        }
    }
}
