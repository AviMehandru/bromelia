import BroDomain
import BroFoundation
import BroTestSupport
import Foundation
import Testing

private func strings(_ s: [String]) -> JsonValue { .array(s.map(JsonValue.string)) }
private func entriesOf(_ v: JsonValue?) -> [SumEntry] {
    (v?.array ?? []).map { SumEntry(path: $0["path"]?.string ?? "", sha256: $0["sha256"]?.string ?? "") }
}
private func entriesJson(_ e: [SumEntry]) -> JsonValue { .array(e.map { .object([("path", .string($0.path)), ("sha256", .string($0.sha256))]) }) }

struct ArchiveFormatTests {
    @Test func archiveFormatCases() throws {
        let failures = try Fixtures.runCases("domain/archive-format.cases.json") { _, given, expect in
            if let bytes = given["bytes"]?.string {
                try Fixtures.same(expect["sha256"]?.string, Sha256Sums.hash(Array(bytes.utf8)), "sha256")
            } else if let existing = given["existing"]?.string {
                try Fixtures.same(expect["text"]?.string, Sha256Sums.merge(existing, entries: entriesOf(given["entries"])), "merged")
            } else if let entries = given["entries"] {
                try Fixtures.same(expect["text"]?.string, Sha256Sums.render(entriesOf(entries)), "text")
            } else if let text = given["text"]?.string {
                try Fixtures.same(expect["entries"], entriesJson(Sha256Sums.parse(text)), "entries")
            } else if let produced = given["produced"]?.array {
                try Fixtures.same(expect["relative"], strings(ArchiveFiles.expand(produced.compactMap(\.string), tree: (given["tree"]?.array ?? []).compactMap(\.string))), "relative")
            } else if let name = given["name"]?.string {
                if let own = expect["isOwnFile"] {
                    try Fixtures.same(own.bool, ArchiveFiles.isOwnFile(name), "own file")
                } else {
                    try Fixtures.same(expect["isMetadataFile"]?.bool, ArchiveFiles.isMetadataFile(name), "metadata file")
                }
            } else if let sums = given["sums"]?.array {
                let hashes = Dictionary(uniqueKeysWithValues: (given["hashes"]?.members ?? []).map { ($0.key, FileVerdict(rawValue: $0.value.string ?? "")!) })
                let r = VerifyResult.compare(sums.compactMap(\.string), hashes: hashes, folderFiles: (given["folder"]?.array ?? []).compactMap(\.string))
                try Fixtures.same(expect["result"]?.string, r.result.rawValue, "result")
                if let error = expect["error"] {
                    try Fixtures.same(error, r.summary.toJson(), "error")
                } else {
                    try Fixtures.same(expect["files"]?.int, Int64(r.files), "files")
                    for (key, list) in [("changed", r.changed), ("missing", r.missing), ("unreadable", r.unreadable), ("unlisted", r.unlisted)] {
                        if let want = expect[key] { try Fixtures.same(want, strings(list), key) }
                    }
                    try Fixtures.same(expect["summary"], r.summary.toJson(), "summary")
                    try Fixtures.same(expect["summaryText"]?.string, try English.render(r.summary.toJson()), "summary text")
                }
            } else {
                return false
            }
            return true
        }
        #expect(failures.isEmpty, "\(failures.joined(separator: "\n"))")
    }

    @Test func version3RecordsRoundTrip() throws {
        for name in ["archive/record3-movie-read-errors.json", "archive/record3-tv-play-all.json"] {
            let bytes = try Fixtures.bytes(name)
            let record = try #require(ArchiveRecordCodec.decode(bytes))
            #expect(record.version == 3)
            #expect(ArchiveRecordCodec.encodeV3(record) == bytes)
            var reversed = record
            reversed.document = .object(Array((record.document.members ?? []).reversed()))
            #expect(ArchiveRecordCodec.encodeV3(reversed) == bytes)
        }
        let movie = try #require(ArchiveRecordCodec.decode(try Fixtures.bytes("archive/record3-movie-read-errors.json")))
        #expect(movie.status == "errors")
        #expect(movie.unitId == "a1b2c3d4-0000-4000-8000-00000000abcd")
        #expect(ArchiveRecordCodec.archivedDisc(movie, folder: "x") == nil)
        let tv = try #require(ArchiveRecordCodec.decode(try Fixtures.bytes("archive/record3-tv-play-all.json")))
        #expect(ArchiveRecordCodec.archivedDisc(tv, folder: "Shows/X")?.lastEpisode == tv.episodes.max())
    }

    @Test func version2RecordsAreRead() throws {
        let r = try #require(ArchiveRecordCodec.decode(try Fixtures.bytes("archive/record2-sample-movie.json")))
        #expect(r.version == 2)
        #expect(r.status == "success" && r.name == "Sample Movie" && r.kind == "movie" && r.label == "SAMPLE_MOVIE")
        #expect(r.fingerprint == "v1:1111111111111111aaaaaaaaaaaaaaaa")
        #expect(r.files.count == 1)
        #expect(r.unitId == nil)
        #expect(ArchiveRecordCodec.decode(Array("{\"format\": \"other\"}".utf8)) == nil)
        #expect(ArchiveRecordCodec.decode(Array("{\"format\": \"bromelia-archive\", \"version\": 4}".utf8)) == nil)
    }

    @Test func notesSayWhyTheFilesAreNotAnArchive() throws {
        let lines = Notes.render(Id("b2c3d4e5-0000-4000-8000-00000000abcd"), outcome: .succeededWithReadErrors,
                                 error: BroError("rip.readErrors", [("count", .integer(1))]),
                                 readErrors: [RobotMessage(code: 2003, flags: 516, text: "Error 'Scsi error - MEDIUM ERROR' occurred while reading")],
                                 logs: ["bromelia-a1b2c3d4-log.txt", "makemkv-a1b2c3d4-log.txt"])
        let text = try lines.map { try English.render($0.toJson()) }.joined(separator: "\n")
        #expect(text == """
            Bromelia job b2c3d4e5-0000-4000-8000-00000000abcd: Completed with read errors.
            These files are NOT a finished archive. Rip the disc again (clean it first if it has read errors),
            or check the files yourself before using them.
            MakeMKV reported 1 read error while reading the disc, so the files may be damaged. They were kept apart from finished archives.
            Errors reported by MakeMKV while reading the disc:
            Error 'Scsi error - MEDIUM ERROR' occurred while reading
            The job's log is in this folder as bromelia-a1b2c3d4-log.txt, and everything MakeMKV printed as makemkv-a1b2c3d4-log.txt.
            """)
    }
}
