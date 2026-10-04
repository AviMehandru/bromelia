import BroAdapters
import BroDomain
import BroFoundation
import BroTestSupport
import Foundation
import Testing

/// shared/fixtures/adapters/video-ts-byte-source.cases.json, and the real DVD ISO when BROMELIA_TEST_DVD_ISO is set.
struct VideoTsByteSourceTests {
    /// What DvdNav.analyse found, in short: each title's number and chapter count, and the jumps.
    func summary(_ source: ByteSource) -> String {
        guard let a = DvdNav.analyse(source) else { return "none" }
        return a.titles.map { "\($0.number):\($0.chapters.count)" }.joined(separator: ",") + " / "
            + a.jumps.map { "\($0.title).\($0.chapter) \($0.how)" }.joined(separator: ",")
    }

    @Test func theSharedCasesPass() throws {
        let doc = try Fixtures.json("adapters/video-ts-byte-source.cases.json")
        var failures: [String] = []
        for c in doc["cases"]?.array ?? [] {
            let given = c["given"]!, expect = c["expect"]!
            do {
                let source = VideoTsByteSource.open(Fixtures.url(given["open"]!.string!).path)
                if expect["opens"]?.bool == false {
                    try Fixtures.check(source == nil, "opened")
                    continue
                }
                guard let source else { throw BroError("not opened") }
                try Fixtures.same(expect["label"]?.string, source.label(), "label")
                if let files = expect["files"]?.array {
                    try Fixtures.same(files.map { "\($0.array![0].string!) \($0.array![1].int!)" }, source.files().map { "\($0.name) \($0.size)" }, "files")
                }
                for r in expect["reads"]?.array ?? [] {
                    let bytes = source.read(r["file"]!.string!, offset: r["offset"]!.int!, length: Int(r["length"]!.int!))
                    if let text = r["text"]?.string { try Fixtures.same(text, String(decoding: bytes, as: UTF8.self), "text") }
                    if let hex = r["hex"]?.string { try Fixtures.same(hex, bytes.map { String(format: "%02x", $0) }.joined(), "hex") }
                    if let count = r["count"]?.int { try Fixtures.same(Int(count), bytes.count, "count") }
                }
                if let other = expect["sameAnalysisAs"]?.string {
                    let folder = VideoTsByteSource.open(Fixtures.url(other).path)!
                    let mine = summary(source)
                    try Fixtures.check(mine != "none", "analysed")
                    try Fixtures.same(summary(folder), mine, "analysis")
                }
            } catch {
                failures.append("\(c["id"]!.string!): \(error)")
            }
        }
        #expect(failures.isEmpty, "\(failures.joined(separator: "\n"))")
    }

    @Test func theRealDvdIsoReadsLikeItsFiles() throws {
        guard let iso = ProcessInfo.processInfo.environment["BROMELIA_TEST_DVD_ISO"], !iso.isEmpty else { return }
        let source = try #require(VideoTsByteSource.open(iso))
        #expect(source.files().contains { $0.name == "VIDEO_TS.IFO" })
        #expect(String(decoding: source.read("VIDEO_TS.IFO", offset: 0, length: 12), as: UTF8.self) == "DVDVIDEO-VMG")
        #expect(summary(source) != "none")
    }
}
