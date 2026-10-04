import BroDomain
import BroFoundation
import BroTestSupport
import Testing

struct NfoTests {
    @Test func nfoCases() throws {
        for c in try Fixtures.json("nfo/nfo-cases.json")["cases"]?.array ?? [] {
            let name = c["nfo"]?.string ?? ""
            let want = try Fixtures.text("nfo/" + name)
            let tv = c["kind"]?.string == "tv"
            let got: String
            if let seasonFile = c["seasonFile"]?.string {
                let bytes = try Fixtures.bytes("lookup/" + seasonFile)
                let season = c["provider"]?.string == "tmdb" ? TmdbParse.season(bytes) : OmdbParse.season(bytes)
                let e = Int(c["episode"]?.int ?? 0)
                got = Nfo.episode(c["show"]?.string ?? "", season: Int(c["season"]?.int ?? 0), episode: e, details: try #require(season[e]))
            } else {
                let kind: MediaKind = tv ? .tv : .movie
                let match: Candidate
                if let details = c["details"]?.string {
                    let bytes = try Fixtures.bytes("lookup/" + details)
                    match = try #require(c["provider"]?.string == "tmdb" ? TmdbParse.details(bytes, kind: kind) : OmdbParse.details(bytes, kind: kind))
                } else {
                    match = Candidate(title: c["match"]?["title"]?.string ?? "", year: nil, tmdbId: nil, imdbId: nil, provider: "")
                }
                got = tv ? Nfo.show(match) : Nfo.movie(match)
            }
            try Fixtures.same(want, got, name)
        }
    }
}
