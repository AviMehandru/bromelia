import BroDomain
import BroFoundation
import BroTestSupport
import Foundation
import Testing

/// A VIDEO_TS folder of the fixtures as a ByteSource (the adapter comes in phase 2).
struct FolderByteSource: ByteSource {
    let dir: URL

    func entries() -> [URL] { (try? FileManager.default.contentsOfDirectory(at: dir, includingPropertiesForKeys: nil)) ?? [] }

    func read(_ path: String, offset: Int64, length: Int) -> [UInt8] {
        guard let url = entries().first(where: { $0.lastPathComponent.uppercased() == path }), let data = try? Data(contentsOf: url) else { return [] }
        let bytes = [UInt8](data)
        guard offset < bytes.count else { return [] }
        return Array(bytes[Int(offset)..<min(bytes.count, Int(offset) + length)])
    }

    func files() -> [ByteFile] {
        entries().map { ByteFile(name: $0.lastPathComponent.uppercased(), size: Int64((try? Data(contentsOf: $0))?.count ?? 0)) }
            .sorted { $0.name.utf8.lexicographicallyPrecedes($1.name.utf8) }
    }
}

/// Files held in memory.
struct MemoryByteSource: ByteSource {
    let contents: [String: [UInt8]]

    func read(_ path: String, offset: Int64, length: Int) -> [UInt8] {
        guard let d = contents[path], offset < d.count else { return [] }
        return Array(d[Int(offset)..<min(d.count, Int(offset) + length)])
    }

    func files() -> [ByteFile] { contents.map { ByteFile(name: $0.key, size: Int64($0.value.count)) } }
}

/// h:mm:ss.fff, as the fixtures write durations.
func hms(_ s: Double) -> String {
    let whole = Int(s)
    let rest = String(format: "%06.3f", s.truncatingRemainder(dividingBy: 60))
    return "\(whole / 3600):\(String(format: "%02d", whole % 3600 / 60)):\(rest)"
}

func firstPlan(_ golden: String) throws -> EpisodePlan {
    let g = try Fixtures.json("domain/" + golden)
    let analysis = try #require(DvdNav.analyse(FolderByteSource(dir: Fixtures.url(g["input"]?.string ?? ""))))
    return try #require(DvdNav.plans(analysis).first)
}

private func ints(_ v: [Int]) -> JsonValue { .array(v.map { .integer(Int64($0)) }) }

/// A menu VOB of 48 sectors: NAV packs at sectors 0 (cell 1), 20 and 30 (cell 2); the first has a button that jumps
/// to title 4.
func menuDisc() -> [String: [UInt8]] {
    var vmg = [UInt8](repeating: 0, count: 0xD0)
    vmg.replaceSubrange(0..<12, with: Array("DVDVIDEO-VMG".utf8))
    var vob = [UInt8](repeating: 0, count: 48 * 2048)
    func nav(_ sector: Int, _ cell: UInt8, _ button: Bool) {
        let o = sector * 2048
        vob[o + 0x10] = 1; vob[o + 0x11] = 0xBB
        vob[o + 0x28] = 1; vob[o + 0x29] = 0xBF
        let dsi = o + 0x407
        vob[dsi + 0x19] = 1
        vob[dsi + 0x1B] = cell
        guard button else { return }
        let pci = o + 0x2D
        vob[pci + 0x60 + 16] = 1
        vob.replaceSubrange((pci + 0x8E + 10)..<(pci + 0x8E + 18), with: [48, 2, 0, 0, 0, 4, 0, 0])
    }
    nav(0, 1, true)
    nav(20, 2, false)
    nav(30, 2, false)
    return ["VIDEO_TS.IFO": vmg, "VIDEO_TS.VOB": vob]
}

struct DvdNavTests {
    @Test func findsPlayAllEpisodes() throws {
        let want = try Fixtures.json("domain/dvd-play-all.navigation.expected.json")["expect"]!["plan"]!
        let plan = try firstPlan("dvd-play-all.navigation.expected.json")
        try Fixtures.same(want["title"]?.int, Int64(plan.title), "title")
        try Fixtures.same(want["starts"], ints(plan.starts), "starts")
        try Fixtures.same(want["lastEnd"]?.int, Int64(plan.lastEnd), "lastEnd")
        try Fixtures.same(want["endRule"]?.string, plan.endRule, "endRule")
        try Fixtures.same(want["tail"], ints(plan.tail), "tail")
        try Fixtures.same(want["splitChapters"], ints(plan.splitChapters), "splitChapters")
        try Fixtures.same(want["duration"]?.string, hms(plan.duration), "duration")
        try Fixtures.same(want["chapterStart8"]?.string, hms(plan.chapterStarts[7]), "chapterStart8")
        for m in want["chapterRanges"]?.members ?? [] {
            let r = DvdNav.chapterRange(plan, episode: Int(m.key)!)
            try Fixtures.same(m.value, ints([r.first, r.last]), "chapterRange \(m.key)")
        }
        try Fixtures.same(want["plausibleStrict"]?.bool, DvdNav.isPlausible(plan, strict: true), "plausibleStrict")
        for m in want["reasons"]?.members ?? [] { try Fixtures.same(m.value.string, plan.reasons[Int(m.key)!], "reason \(m.key)") }
        #expect(DvdNav.matchesChapterCount(plan, chapters: plan.keptChapters))
        #expect(!DvdNav.matchesChapterCount(plan, chapters: plan.keptChapters - 2))
    }

    @Test func navigationCases() throws {
        let failures = try Fixtures.runCases("domain/dvdnav.cases.json") { _, given, expect in
            if let bytes = given["bytes"]?.array {
                let got: JsonValue
                switch DvdNav.decodeJump(bytes.map { UInt8($0.int ?? 0) }) {
                case .ptt(let n, let chapter, let condition)?:
                    got = .object([("kind", .string("ptt")), ("titleNumber", n.map { .integer(Int64($0)) } ?? .null),
                                   ("ptt", .integer(Int64(chapter))), ("condition", .string(condition))])
                case .title(let n, let condition)?:
                    got = .object([("kind", .string("title")), ("title", .integer(Int64(n))), ("condition", .string(condition))])
                case nil:
                    got = .null
                }
                try Fixtures.same(expect["jump"], got, "jump")
                return true
            }
            let plan = try firstPlan(given["plan"]?.string ?? "")
            let split = (given["split"]?.array ?? []).map { Int($0.int ?? 0) }
            var starts: [Double]?
            if given["mkvStarts"]?.string != nil {
                // "a chapter 00 at 0, then every disc chapter start + 0.02 s"
                starts = [0] + plan.chapterStarts.map { $0 + 0.02 }
            } else if let a = given["mkvStarts"]?.array {
                starts = a.map { $0.double ?? 0 }
            }
            let chapters = DvdNav.mkvChapters(split, plan: plan, mkvStarts: starts)
            try Fixtures.same(expect["chapters"], chapters.map(ints) ?? .null, "chapters")
            return true
        }
        #expect(failures.isEmpty, "\(failures.joined(separator: "\n"))")
    }

    @Test func menuStillsAndButtons() throws {
        let a = try #require(DvdNav.analyse(MemoryByteSource(contents: menuDisc())))
        #expect(DvdNav.stillCells(a) == [CellRef(file: "VIDEO_TS.VOB", firstSector: 0, endSector: 20), CellRef(file: "VIDEO_TS.VOB", firstSector: 20, endSector: 48)])
        #expect(a.jumps == [NavJump(title: 4, chapter: 1, how: "button JumpTT")])
        #expect(a.titles.isEmpty)
        #expect(DvdNav.analyse(MemoryByteSource(contents: ["VIDEO_TS.IFO": [UInt8](repeating: 0, count: 0x100)])) == nil)
    }
}
