import Testing
import Foundation
@testable import Bromelia

// Tests for the safeguards that keep damaged, incomplete or wrong rips out of the archive:
// read errors, staging and marked folders, checks against the disc listing, and disc changes.

/// A disc listing in makemkvcon's robot format. Each title is (duration, source title id).
private func listing(volume: String = "SAMPLE_MOVIE", titles: [(String, Int)]) -> String {
    var s = """
    MSG:1005,0,1,"MakeMKV v1.18.1 darwin(arm64-release) started","%1 started","MakeMKV v1.18.1 darwin(arm64-release)"
    TCOUNT:\(titles.count)
    CINFO:1,6209,"Blu-ray disc"
    CINFO:2,0,"\(volume)"
    CINFO:32,0,"\(volume)"

    """
    for (i, t) in titles.enumerated() {
        s += """
        TINFO:\(i),8,0,"2"
        TINFO:\(i),9,0,"\(t.0)"
        TINFO:\(i),16,0,"0000\(t.1).mpls"
        TINFO:\(i),24,0,"\(t.1)"
        TINFO:\(i),26,0,"\(t.1)"
        TINFO:\(i),27,0,"title_t0\(i).mkv"
        SINFO:\(i),0,1,6201,"Video"
        SINFO:\(i),1,1,6202,"Audio"

        """
    }
    return s + "MSG:5011,0,0,\"Operation successfully completed\",\"Operation successfully completed\"\n"
}

private func info(_ text: String) -> DiscInfo { DiscInfoBuilder.build(fromOutput: text) }

private let readError = #"MSG:2003,0,3,"Error 'Scsi error - MEDIUM ERROR:L-EC UNCORRECTABLE ERROR' occurred while reading '/BDMV/STREAM/00001.m2ts' at offset '1048576'","Error '%1' occurred while reading '%2' at offset '%3'","Scsi error - MEDIUM ERROR:L-EC UNCORRECTABLE ERROR","/BDMV/STREAM/00001.m2ts","1048576""#
private let saved = #"MSG:5036,260,1,"Copy complete. 1 titles saved.","Copy complete. %1 titles saved.","1""#
private let failedSave = """
MSG:5003,0,2,"Failed to save title 1 to file title_t01.mkv","Failed to save title %1 to file %2","1","title_t01.mkv"
MSG:5037,516,2,"Copy complete. 0 titles saved, 1 failed.","Copy complete. %1 titles saved, %2 failed.","0","1"
"""

/// A stand-in for makemkvcon: `info` prints a listing, `mkv` runs the scenario's shell code for the title.
/// Every call is recorded in `calls.txt`.
private struct FakeMakeMKV {
    let dir: URL
    var executable: URL { dir.appendingPathComponent("makemkvcon") }
    var calls: String { (try? String(contentsOf: dir.appendingPathComponent("calls.txt"), encoding: .utf8)) ?? "" }

    /// `mkv` is shell code run with `$title` and `$dest` set; `backup` with `$dest` set.
    init(listing: String, mkv: String, backup: String = "") throws {
        dir = FileManager.default.temporaryDirectory.appendingPathComponent("bromelia-fake-\(UUID().uuidString.prefix(8))", isDirectory: true)
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        try listing.write(to: dir.appendingPathComponent("listing.txt"), atomically: true, encoding: .utf8)
        let mkv = mkv.replacingOccurrences(of: "__LISTING__", with: dir.appendingPathComponent("listing.txt").path)
        let script = """
        #!/bin/sh
        echo "$@" >> '\(dir.path)/calls.txt'
        while [ $# -gt 0 ]; do
          case "$1" in info|mkv|backup) cmd="$1"; shift; break;; esac
          shift
        done
        case "$cmd" in
          info) cat '\(dir.path)/listing.txt' ;;
          mkv) title="$2"; dest="$3"
        \(mkv)
          ;;
          backup) for a in "$@"; do dest="$a"; done
        \(backup)
          ;;
        esac
        exit 0
        """
        try script.write(to: executable, atomically: true, encoding: .utf8)
        try FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: executable.path)
    }
}

/// Shell code that writes a small file as MakeMKV's output for the title (for `all`: titles 0 to 4 that
/// are in the listing), then prints `messages`.
private func writesFile(_ messages: String) -> String {
    """
        if [ "$title" = "all" ]; then
          for t in 0 1 2 3 4; do grep -q "title_t0$t.mkv" '__LISTING__' && printf 'mkv data' > "$dest/title_t0$t.mkv"; done
        else
          printf 'mkv data %s' "$title" > "$dest/title_t0$title.mkv"
        fi
    cat <<'EOF'
    \(messages)
    EOF
    """
}

@Suite("Rip safeguards", .serialized)
@MainActor
struct SafetyTests {
    let root: URL

    init() throws {
        Paths.dataOverride = FileManager.default.temporaryDirectory.appendingPathComponent("bromelia-test-data", isDirectory: true)
        root = FileManager.default.temporaryDirectory.appendingPathComponent("bromelia-safety-\(UUID().uuidString.prefix(8))", isDirectory: true)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
    }

    private func run(_ fake: FakeMakeMKV, titles: [Int]? = nil, opened: DiscInfo? = nil, mkvmerge: URL? = nil,
                     configure: (inout DriveConfig) -> Void = { _ in }) async -> RipJob {
        var config = AppConfig()
        config.outputRoot = root.path
        var drive = DriveConfig()
        drive.name = "Test drive"
        drive.automation.notify = false
        drive.rip.titleSelection.skipDuplicates = false
        configure(&drive)
        let job = RipJob(source: .iso(path: "/nonexistent/test.iso"), drive: drive, laneKey: "iso:test", sourceLabel: "test", discLabel: "", mode: .mkv)
        job.manualTitles = titles
        job.preloadedInfo = opened
        let runner = JobRunner(job: job, config: config, makemkvcon: fake.executable, mkvmerge: mkvmerge)
        await runner.run()
        return job
    }

    private var rootItems: [String] { JobRunner.visibleItems(root) }

    private func allItems(_ dir: URL) -> [String] { ((try? FileManager.default.contentsOfDirectory(atPath: dir.path)) ?? []).sorted() }

    @Test func successfulRipIsMovedIntoTheOutputFolder() async throws {
        let fake = try FakeMakeMKV(listing: listing(titles: [("0:00:10", 1), ("0:00:20", 2)]), mkv: writesFile(saved))
        let job = await run(fake, titles: [0, 1])

        #expect(job.state == .succeeded, "\(job.errorMessage ?? "")")
        let out = try #require(job.outputDirectory)
        #expect(out.lastPathComponent == "Sample Movie")
        #expect(rootItems == ["Sample Movie"])
        let items = allItems(out)
        #expect(!items.contains { $0.hasPrefix(".bromelia") }, "staging folder left behind: \(items)")
        #expect(items.contains("SHA256SUMS") && items.contains("bromelia.json"))
        #expect(items.contains("bromelia-log.txt") && items.contains("makemkv-log.txt"), "\(items)")
        #expect(!items.contains("makemkv-debug-log.txt"), "MakeMKV's debug log is off")
        // Every line makemkvcon printed, run after run, not only its messages.
        let raw = try String(contentsOf: out.appendingPathComponent("makemkv-log.txt"), encoding: .utf8)
        #expect(raw.components(separatedBy: "\n").filter { $0.hasPrefix("==== ") && $0.contains(" $ ") }.count == 2, "\(raw)")
        #expect(raw.contains("\nTCOUNT:2\n") && raw.contains("\nTINFO:1,27,0,\"title_t01.mkv\"\n") && raw.contains("Copy complete. 1 titles saved."))
        #expect(raw.contains("exit status 0"))
        #expect(job.producedFiles.count == 2)
        for f in job.producedFiles { #expect(f.deletingLastPathComponent().path == out.path && FileManager.default.fileExists(atPath: f.path)) }
        #expect(try Checksums.verify(folder: out).isEmpty)
        let record = try JSONSerialization.jsonObject(with: Data(contentsOf: out.appendingPathComponent("bromelia.json"))) as? [String: Any]
        #expect(record?["status"] as? String == "success")
    }

    @Test func readErrorsKeepTheFilesApart() async throws {
        let fake = try FakeMakeMKV(listing: listing(titles: [("0:00:10", 1)]), mkv: writesFile(readError + "\n" + saved))
        let job = await run(fake, titles: [0])

        #expect(job.state == .completedWithErrors)
        #expect(job.dataErrors.count == 1)
        #expect(rootItems == ["Sample Movie [READ ERRORS]"], "\(rootItems)")
        let out = try #require(job.outputDirectory)
        #expect(out.lastPathComponent == "Sample Movie [READ ERRORS]")
        let items = allItems(out)
        #expect(items.contains("READ ERRORS.txt") && items.contains("SHA256SUMS") && items.contains("bromelia.json"), "\(items)")
        #expect(items.filter { $0.hasSuffix(".mkv") }.count == 1)
        let note = try String(contentsOf: out.appendingPathComponent("READ ERRORS.txt"), encoding: .utf8)
        #expect(note.contains("NOT a finished archive") && note.contains("MEDIUM ERROR"))
        let record = try JSONSerialization.jsonObject(with: Data(contentsOf: out.appendingPathComponent("bromelia.json"))) as? [String: Any]
        #expect(record?["status"] as? String == "errors")
        #expect((record?["readErrors"] as? [String])?.count == 1)
        let manifest = try ConfigStore.decoder().decode(JobManifest.self, from: Data(contentsOf: job.manifestFile))
        #expect(manifest.status == "errors")
    }

    @Test func listingErrorsAreNotReadErrors() async throws {
        // Errors while reading the listing (before the rip) don't mark the rip as damaged.
        let fake = try FakeMakeMKV(listing: readError + "\n" + listing(titles: [("0:00:10", 1)]), mkv: writesFile(saved))
        let job = await run(fake, titles: [0])
        #expect(job.state == .succeeded, "\(job.errorMessage ?? "")")
        #expect(job.dataErrors.isEmpty)
    }

    @Test func failedTitleIsKeptUnderMakeMKVsNameInAnIncompleteFolder() async throws {
        let mkv = """
            printf 'partial' > "$dest/title_t0$title.mkv"
            if [ "$title" = "1" ]; then cat <<'EOF'
        \(failedSave)
        EOF
            else cat <<'EOF'
        \(saved)
        EOF
            fi
        """
        let fake = try FakeMakeMKV(listing: listing(titles: [("0:00:10", 1), ("0:00:20", 2), ("0:00:30", 3)]), mkv: mkv)
        let job = await run(fake, titles: [0, 1])

        #expect(job.state == .failed)
        #expect(rootItems == ["Sample Movie [INCOMPLETE]"], "\(rootItems)")
        let out = try #require(job.outputDirectory)
        let items = allItems(out)
        #expect(items.contains("INCOMPLETE.txt"))
        #expect(items.contains("title_t01.mkv"), "the failed title must keep MakeMKV's name: \(items)")
        #expect(!items.contains("SHA256SUMS"))
        #expect(!items.contains { $0.hasPrefix(".bromelia") })
    }

    @Test func failureWithoutFilesLeavesNoFolder() async throws {
        let fake = try FakeMakeMKV(listing: listing(titles: [("0:00:10", 1)]), mkv: "cat <<'EOF'\n\(failedSave)\nEOF")
        let job = await run(fake, titles: [0])
        #expect(job.state == .failed)
        #expect(allItems(root).isEmpty, "\(allItems(root))")
        #expect(job.outputDirectory == nil)
    }

    @Test func failureInASharedFolderUsesASubfolder() async throws {
        try "keep".write(to: root.appendingPathComponent("other.mkv"), atomically: true, encoding: .utf8)
        let fake = try FakeMakeMKV(listing: listing(titles: [("0:00:10", 1)]), mkv: writesFile(failedSave))
        let job = await run(fake, titles: [0]) {
            $0.output.folderTemplate = ""
            $0.output.conflictPolicy = .overwrite
        }
        #expect(job.state == .failed)
        let items = rootItems
        #expect(items.count == 2 && items.contains("other.mkv"), "\(items)")
        let sub = try #require(items.first { $0.hasPrefix("INCOMPLETE - ") })
        let kept = allItems(root.appendingPathComponent(sub))
        #expect(kept.contains("title_t00.mkv"), "\(kept)")
        // The logs go with the files that were kept.
        #expect(kept.contains("bromelia-log.txt") && kept.contains("makemkv-log.txt"), "\(kept)")
        let note = try String(contentsOf: root.appendingPathComponent(sub).appendingPathComponent("INCOMPLETE.txt"), encoding: .utf8)
        #expect(note.contains("bromelia-log.txt") && note.contains("makemkv-log.txt"))
        let raw = try String(contentsOf: root.appendingPathComponent(sub).appendingPathComponent("makemkv-log.txt"), encoding: .utf8)
        #expect(raw.contains("Failed to save title 1 to file title_t01.mkv"))
    }

    @Test func logsFollowFilesOnlyWithTheArchiveRecord() async throws {
        let fake = try FakeMakeMKV(listing: listing(titles: [("0:00:10", 1)]), mkv: writesFile(failedSave))
        let job = await run(fake, titles: [0]) { $0.archive.archiveRecord = false }
        #expect(job.state == .failed)
        let out = try #require(job.outputDirectory)
        let items = allItems(out)
        #expect(items.contains("title_t00.mkv") && !items.contains { $0.hasSuffix("-log.txt") }, "\(items)")
        // The job folder keeps them anyway.
        #expect(FileManager.default.fileExists(atPath: job.makemkvLogFile.path))
        let note = try String(contentsOf: out.appendingPathComponent("INCOMPLETE.txt"), encoding: .utf8)
        #expect(!note.contains("bromelia-log.txt") && note.contains(job.logFile.path))
    }

    @Test func makeMKVsDebugLogIsKeptAfterEachRun() async throws {
        // makemkvcon names its debug log in message 1004 and writes it anew on every start.
        let dir = FileManager.default.temporaryDirectory.appendingPathComponent("bromelia-debug \(UUID().uuidString.prefix(6))", isDirectory: true)
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: dir) }
        let debugLog = dir.appendingPathComponent("MakeMKV_log.txt")
        try "Debug log of the listing\n".write(to: debugLog, atomically: true, encoding: .utf8)
        let uri = "file://" + debugLog.path.replacingOccurrences(of: " ", with: "%20")
        let announce = #"MSG:1004,131072,1,"Debug logging enabled, log will be saved as \#(uri)","Debug logging enabled, log will be saved as %1","\#(uri)""#
        let rip = "    printf 'Debug log of the rip' > '\(debugLog.path)'\n" + writesFile(announce + "\n" + saved)
        let fake = try FakeMakeMKV(listing: announce + "\n" + listing(titles: [("0:00:10", 1)]), mkv: rip)
        let job = await run(fake, titles: [0])

        #expect(job.state == .succeeded, "\(job.errorMessage ?? "")")
        let kept = try String(contentsOf: job.makemkvDebugLogFile, encoding: .utf8)
        let listingAt = try #require(kept.range(of: "Debug log of the listing\n"))
        let ripAt = try #require(kept.range(of: "Debug log of the rip\n"))
        #expect(listingAt.lowerBound < ripAt.lowerBound)
        #expect(kept.components(separatedBy: "\n").filter { $0.hasPrefix("==== \(debugLog.path) after $ ") }.count == 2, "\(kept)")
        let out = try #require(job.outputDirectory)
        #expect(try String(contentsOf: out.appendingPathComponent("makemkv-debug-log.txt"), encoding: .utf8) == kept)
    }

    @Test func aDifferentDiscIsNotRipped() async throws {
        let fake = try FakeMakeMKV(listing: listing(titles: [("0:00:10", 1)]), mkv: writesFile(saved))
        let opened = info(listing(volume: "OTHER_DISC", titles: [("0:00:10", 1)]))
        let job = await run(fake, titles: [0], opened: opened)
        #expect(job.state == .failed)
        #expect(job.errorMessage?.contains("not the disc that was opened") == true, "\(job.errorMessage ?? "")")
        #expect(!fake.calls.contains(" mkv "))
        #expect(allItems(root).isEmpty)
    }

    @Test func chosenTitlesFollowChangedTitleNumbers() async throws {
        // The listing now has an extra title first (e.g. a lower minimum title length), so title 1 became 2.
        let fake = try FakeMakeMKV(listing: listing(titles: [("0:00:05", 9), ("0:00:10", 1), ("0:00:20", 2)]), mkv: writesFile(saved))
        let opened = info(listing(titles: [("0:00:10", 1), ("0:00:20", 2)]))
        let job = await run(fake, titles: [1], opened: opened)
        #expect(job.state == .succeeded, "\(job.errorMessage ?? "")")
        #expect(job.manualTitles == [2])
        #expect(fake.calls.contains("mkv iso:/nonexistent/test.iso 2 "), "\(fake.calls)")
        #expect(!fake.calls.contains("mkv iso:/nonexistent/test.iso 1 "))
    }

    @Test func aChosenTitleMissingFromTheListingFailsTheJob() async throws {
        let fake = try FakeMakeMKV(listing: listing(titles: [("0:00:10", 1)]), mkv: writesFile(saved))
        let opened = info(listing(titles: [("0:00:10", 1), ("0:00:20", 2)]))
        let job = await run(fake, titles: [1], opened: opened)
        #expect(job.state == .failed)
        #expect(job.errorMessage?.contains("not in the new disc listing") == true, "\(job.errorMessage ?? "")")
        #expect(!fake.calls.contains(" mkv "))
    }

    nonisolated static let tools: (ffmpeg: URL, mkvmerge: URL)? = {
        guard let f = EpisodeSplitter.findTool("ffmpeg"), let m = Paths.resolveTool(configured: "", candidates: Paths.mkvmergeCandidates) else { return nil }
        return (f, m)
    }()

    /// A real MKV of `seconds` length with one video and one audio track.
    private func sample(seconds: Int) async throws -> URL {
        let (ffmpeg, _) = try #require(Self.tools)
        let url = FileManager.default.temporaryDirectory.appendingPathComponent("bromelia-sample-\(seconds)-\(UUID().uuidString.prefix(6)).mkv")
        let r = ProcessRunner(executable: ffmpeg, arguments: ["-v", "error", "-y", "-f", "lavfi", "-i", "testsrc=size=64x48:rate=5:duration=\(seconds)",
                                                             "-f", "lavfi", "-i", "sine=duration=\(seconds)", "-c:v", "mpeg4", "-c:a", "aac", url.path])
        let out = try await r.run { _ in }
        try #require(out.exitCode == 0)
        return url
    }

    @Test(.enabled(if: SafetyTests.tools != nil, "needs ffmpeg and mkvmerge"))
    func ripsAreCheckedAgainstTheListing() async throws {
        let good = try await sample(seconds: 10), short = try await sample(seconds: 3)
        // Title 0 is complete; title 1 lasts 3 s instead of 20 s.
        let mkv = """
            if [ "$title" = "0" ]; then cp '\(good.path)' "$dest/title_t00.mkv"; else cp '\(short.path)' "$dest/title_t01.mkv"; fi
            cat <<'EOF'
        \(saved)
        EOF
        """
        let fake = try FakeMakeMKV(listing: listing(titles: [("0:00:10", 1), ("0:00:20", 2), ("0:00:30", 3)]), mkv: mkv)
        let job = await run(fake, titles: [0, 1], mkvmerge: Self.tools?.mkvmerge)
        #expect(job.state == .failed)
        #expect(job.errorMessage?.contains("doesn't match the disc listing") == true, "\(job.errorMessage ?? "")")
        #expect(job.log.contains { $0.text.hasPrefix("Checked ") }, "title 0 should pass the check")
        #expect(rootItems == ["Sample Movie [INCOMPLETE]"])
        #expect(allItems(root.appendingPathComponent("Sample Movie [INCOMPLETE]")).contains("title_t01.mkv"))
    }
}

@Suite("Rip checks")
struct RipCheckTests {
    private func title(_ duration: String, chapters: Int = 0, tracks: [String] = ["Video", "Audio"]) -> TitleInfo {
        var t = TitleInfo(index: 0)
        t.attributes[AttributeID.duration.rawValue] = duration
        t.attributes[AttributeID.chapterCount.rawValue] = String(chapters)
        t.tracks = tracks.enumerated().map { TrackInfo(index: $0.offset, attributes: [AttributeID.type.rawValue: $0.element]) }
        return t
    }

    @Test func parsesMkvmergeJSON() throws {
        let json = #"{"container":{"recognized":true,"supported":true,"properties":{"duration":7212345000000}},"tracks":[{"id":0,"type":"video"},{"id":1,"type":"audio"},{"id":2,"type":"subtitles"}],"chapters":[{"num_entries":24}]}"#
        let p = try #require(RipVerifier.parse(json: Data(json.utf8)))
        #expect(p.trackTypes == ["video", "audio", "subtitles"])
        #expect(p.chapterCount == 24)
        #expect(abs(p.durationSeconds! - 7212.345) < 0.001)
        #expect(RipVerifier.parse(json: Data(#"{"container":{"recognized":false}}"#.utf8)) == nil)
    }

    @Test func acceptsMatchingFiles() {
        let r = RipVerifier.check(.init(durationSeconds: 7212.4, trackTypes: ["video", "audio"], chapterCount: 25), against: title("2:00:10", chapters: 24))
        #expect(r.problems.isEmpty && r.notes.isEmpty)
    }

    @Test func rejectsTruncatedOrWrongTitles() {
        #expect(!RipVerifier.check(.init(durationSeconds: 5400, trackTypes: ["video", "audio"], chapterCount: 24), against: title("2:00:10")).problems.isEmpty)
        #expect(!RipVerifier.check(.init(durationSeconds: nil, trackTypes: ["video"], chapterCount: 0), against: title("2:00:10")).problems.isEmpty)
        #expect(!RipVerifier.check(.init(durationSeconds: 7210, trackTypes: [], chapterCount: 0), against: title("2:00:10")).problems.isEmpty)
        #expect(!RipVerifier.check(.init(durationSeconds: 7210, trackTypes: ["audio"], chapterCount: 0), against: title("2:00:10")).problems.isEmpty)
        // Within the tolerance (0.5 % of two hours = 36 s).
        #expect(RipVerifier.check(.init(durationSeconds: 7240, trackTypes: ["video"], chapterCount: 0), against: title("2:00:10")).problems.isEmpty)
        #expect(!RipVerifier.check(.init(durationSeconds: 7260, trackTypes: ["video"], chapterCount: 0), against: title("2:00:10")).problems.isEmpty)
    }

    @Test func notesChapterDifferences() {
        let r = RipVerifier.check(.init(durationSeconds: 600, trackTypes: ["video"], chapterCount: 3), against: title("0:10:00", chapters: 12))
        #expect(r.problems.isEmpty)
        #expect(r.notes.count == 1)
    }

    @Test func matchesTitlesAcrossListings() throws {
        func t(_ i: Int, _ d: String, _ src: Int) -> TitleInfo {
            var x = TitleInfo(index: i)
            x.attributes[AttributeID.duration.rawValue] = d
            x.attributes[AttributeID.originalTitleId.rawValue] = String(src)
            x.tracks = [TrackInfo(index: 0, attributes: [AttributeID.type.rawValue: "Video"])]
            return x
        }
        var old = DiscInfo(), new = DiscInfo()
        old.titles = [t(0, "1:00:00", 1), t(1, "0:20:00", 2)]
        new.titles = [t(0, "0:01:00", 7), t(1, "1:00:00", 1), t(2, "0:20:00", 2)]
        #expect(try ListingMatcher.map([0, 1], from: old, to: new, sameTracks: []) == [0: 1, 1: 2])
        #expect(throws: JobError.self) { try ListingMatcher.map([0], from: old, to: DiscInfo(), sameTracks: []) }
        var changed = new
        changed.titles[1].tracks.append(TrackInfo(index: 1, attributes: [AttributeID.type.rawValue: "Audio"]))
        #expect(throws: JobError.self) { try ListingMatcher.map([0], from: old, to: changed, sameTracks: [0]) }
        #expect(ListingMatcher.differentDisc(old: old, new: new) == nil)
        var other = DiscInfo()
        other.titles = [t(0, "0:45:00", 3)]
        #expect(ListingMatcher.differentDisc(old: old, new: other) != nil)
        old.attributes[AttributeID.volumeName.rawValue] = "A"
        new.attributes[AttributeID.volumeName.rawValue] = "B"
        #expect(ListingMatcher.differentDisc(old: old, new: new) != nil)
    }

    @Test func checksBackupStructure() throws {
        let fm = FileManager.default
        let dir = fm.temporaryDirectory.appendingPathComponent("bromelia-backup-\(UUID().uuidString.prefix(6))", isDirectory: true)
        defer { try? fm.removeItem(at: dir) }
        let bd = dir.appendingPathComponent("bd", isDirectory: true)
        try fm.createDirectory(at: bd.appendingPathComponent("BDMV"), withIntermediateDirectories: true)
        #expect(BackupVerifier.problem(bd, iso: false) != nil)
        try Data().write(to: bd.appendingPathComponent("BDMV/index.bdmv"))
        #expect(BackupVerifier.problem(bd, iso: false) == nil)
        let dvd = dir.appendingPathComponent("dvd", isDirectory: true)
        try fm.createDirectory(at: dvd.appendingPathComponent("VIDEO_TS"), withIntermediateDirectories: true)
        try Data().write(to: dvd.appendingPathComponent("VIDEO_TS/VIDEO_TS.IFO"))
        #expect(BackupVerifier.problem(dvd, iso: false) == nil)
        #expect(BackupVerifier.problem(dir.appendingPathComponent("missing"), iso: false) != nil)

        let fakeISO = dir.appendingPathComponent("folder.iso", isDirectory: true)
        try fm.createDirectory(at: fakeISO, withIntermediateDirectories: true)
        #expect(BackupVerifier.problem(fakeISO, iso: true) != nil)
        var image = Data(count: 40_000)
        image.replaceSubrange(32769..<32774, with: Data("BEA01".utf8))
        let iso = dir.appendingPathComponent("disc.iso")
        try image.write(to: iso)
        #expect(BackupVerifier.problem(iso, iso: true) == nil)
        try Data(count: 40_000).write(to: iso)
        #expect(BackupVerifier.problem(iso, iso: true) != nil)
    }

    @Test func stepsForJobsWithReadErrors() {
        var step = PostProcessStep()
        step.executable = "/bin/true"
        step.runOn = .success
        #expect(!PostProcessor.shouldRun(step, status: .completedWithErrors))
        step.runOn = .failure
        #expect(PostProcessor.shouldRun(step, status: .completedWithErrors))
        #expect(JobState.completedWithErrors.statusWord == "errors")
        #expect(JobState.completedWithErrors.isFinished)
    }
}

// MARK: - Real makemkvcon failures, stalls, free space, ISO backups, presets

private let fixturesDir = URL(fileURLWithPath: #filePath).deletingLastPathComponent().deletingLastPathComponent()
    .deletingLastPathComponent().appendingPathComponent("shared/fixtures")

/// shared/fixtures/rip-outcomes.json: how a job must end for each recorded makemkvcon run. The same file is
/// replayed by the Windows and Linux tests, so the three implementations can't drift apart.
private struct Outcomes: Decodable {
    struct Case: Decodable {
        var fixture: String
        var exitCode: Int
        var writesFile: Bool
        var status: String
        var error: String?
        var keptApart: Bool
    }
    var cases: [Case]
}

@Suite("Recorded makemkvcon runs", .serialized)
@MainActor
struct RecordedRunTests {
    init() {
        Paths.dataOverride = FileManager.default.temporaryDirectory.appendingPathComponent("bromelia-test-data", isDirectory: true)
    }

    @Test func everyRecordedRunEndsAsExpected() async throws {
        let outcomes = try JSONDecoder().decode(Outcomes.self, from: Data(contentsOf: fixturesDir.appendingPathComponent("rip-outcomes.json")))
        #expect(outcomes.cases.count >= 7)
        for c in outcomes.cases {
            let root = FileManager.default.temporaryDirectory.appendingPathComponent("bromelia-rec-\(UUID().uuidString.prefix(8))", isDirectory: true)
            try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
            let fixture = fixturesDir.appendingPathComponent("\(c.fixture).txt").path
            let mkv = """
                \(c.writesFile ? #"printf 'mkv data' > "$dest/title_t0$title.mkv""# : ":")
                cat '\(fixture)'
                exit \(c.exitCode)
            """
            let fake = try FakeMakeMKV(listing: listing(titles: [("0:00:10", 1), ("0:00:20", 2)]), mkv: mkv)
            var config = AppConfig()
            config.outputRoot = root.path
            var drive = DriveConfig()
            drive.automation.notify = false
            let job = RipJob(source: .iso(path: "/nonexistent/test.iso"), drive: drive, laneKey: "iso:rec", sourceLabel: "test", discLabel: "", mode: .mkv)
            job.manualTitles = [0]
            await JobRunner(job: job, config: config, makemkvcon: fake.executable, mkvmerge: nil).run()

            #expect(job.state.statusWord == c.status, "\(c.fixture): \(job.state) \(job.errorMessage ?? "")")
            if let e = c.error {
                #expect(job.errorMessage?.localizedCaseInsensitiveContains(e) == true, "\(c.fixture): \(job.errorMessage ?? "no error")")
            }
            let items = JobRunner.visibleItems(root)
            if c.status == "success" {
                #expect(items == ["Sample Movie"], "\(c.fixture): \(items)")
            } else if c.keptApart {
                #expect(items.count == 1 && items[0].hasSuffix("]"), "\(c.fixture): \(items)")
                let kept = JobRunner.visibleItems(root.appendingPathComponent(items.first ?? ""))
                #expect(kept.contains { $0.hasSuffix(".mkv") }, "\(c.fixture): \(kept)")
            } else {
                #expect(items.isEmpty, "\(c.fixture): nothing should be left, found \(items)")
            }
        }
    }

    @Test func failuresNameTheirCause() throws {
        // The first specific error of a run explains it better than the "0 titles saved, 1 failed" summary.
        func reason(_ fixture: String) throws -> String? {
            let text = try String(contentsOf: fixturesDir.appendingPathComponent("\(fixture).txt"), encoding: .utf8)
            var first: String?
            text.enumerateLines { line, stop in
                if case .message(let m)? = RobotParser.parse(line: line), m.severity == .error, m.code != 5037, m.code != 5004 { first = m.text; stop = true }
            }
            return first
        }
        #expect(try reason("rip-disk-full")?.contains("megabytes free on the destination") == true)
        #expect(try reason("rip-missing-source")?.contains("does not exist") == true)
        #expect(try reason("rip-failure")?.contains("MEDIUM ERROR") == true)
    }
}

@Suite("Stuck processes, free space, ISO backups, presets", .serialized)
@MainActor
struct ReliabilityTests {
    let root: URL

    init() throws {
        Paths.dataOverride = FileManager.default.temporaryDirectory.appendingPathComponent("bromelia-test-data", isDirectory: true)
        root = FileManager.default.temporaryDirectory.appendingPathComponent("bromelia-rel-\(UUID().uuidString.prefix(8))", isDirectory: true)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
    }

    @Test func stuckProcessIsStopped() async throws {
        let start = Date()
        let runner = ProcessRunner(executable: URL(fileURLWithPath: "/bin/sh"), arguments: ["-c", "echo started; sleep 60"], stopSignal: SIGTERM)
        let lines = LineCollector()
        let out = try await runner.run(stallTimeout: 2) { lines.append($0) }
        #expect(out.stalled)
        #expect(out.exitCode != 0)
        #expect(Date().timeIntervalSince(start) < 15)
        #expect(lines.all == ["started"])
    }

    @Test func busyProcessIsNotStopped() async throws {
        let runner = ProcessRunner(executable: URL(fileURLWithPath: "/bin/sh"), arguments: ["-c", "for i in 1 2 3 4 5 6; do echo $i; sleep 0.5; done"])
        let out = try await runner.run(stallTimeout: 2) { _ in }
        #expect(!out.stalled)
        #expect(out.exitCode == 0)
    }

    @Test func notEnoughFreeSpaceStopsBeforeRipping() async throws {
        var text = listing(titles: [("0:00:10", 1)])
        text += "TINFO:0,11,0,\"\(Int64(1) << 60)\"\n"   // an exabyte
        let fake = try FakeMakeMKV(listing: text, mkv: writesFile(saved))
        var config = AppConfig()
        config.outputRoot = root.path
        var drive = DriveConfig()
        drive.automation.notify = false
        let job = RipJob(source: .iso(path: "/nonexistent/test.iso"), drive: drive, laneKey: "iso:space", sourceLabel: "test", discLabel: "", mode: .mkv)
        job.manualTitles = [0]
        await JobRunner(job: job, config: config, makemkvcon: fake.executable, mkvmerge: nil).run()
        #expect(job.state == .failed)
        #expect(job.errorMessage?.contains("Not enough free space") == true, "\(job.errorMessage ?? "")")
        #expect(!fake.calls.contains(" mkv "))
        #expect(JobRunner.visibleItems(root).isEmpty)
    }

    @Test func requiredSpaceHasAMargin() {
        #expect(DiskSpace.required(1_000) == 1_000 + (256 << 20))
        #expect(DiskSpace.required(100 << 30) == (100 << 30) + (2 << 30))
        #expect(DiskSpace.available(at: root) ?? 0 > 0)
    }

    private func backupJob(_ fake: FakeMakeMKV, mode: RipMode) async -> RipJob {
        var config = AppConfig()
        config.outputRoot = root.path
        var drive = DriveConfig()
        drive.automation.notify = false
        drive.automation.ejectWhenDone = false
        drive.rip.backupFormat = .iso
        let job = RipJob(source: .drive(index: 0, devicePath: ""), drive: drive, laneKey: "drive:test", sourceLabel: "test", discLabel: "", mode: mode)
        await JobRunner(job: job, config: config, makemkvcon: fake.executable, mkvmerge: nil).run()
        return job
    }

    @Test func isoBackupIsKept() async throws {
        let backup = #"""
            head -c 40000 /dev/zero > "$dest"
            printf 'BEA01' | dd of="$dest" bs=1 seek=32769 conv=notrunc 2>/dev/null
            echo 'MSG:5036,260,1,"Copy complete. 1 titles saved.","Copy complete. %1 titles saved.","1"'
        """#
        let fake = try FakeMakeMKV(listing: listing(titles: [("0:00:10", 1)]), mkv: ":", backup: backup)
        let job = await backupJob(fake, mode: .backupDecrypted)
        #expect(job.state == .succeeded, "\(job.errorMessage ?? "")")
        #expect(job.producedFiles.first?.pathExtension == "iso")
    }

    @Test func folderWrittenInsteadOfAnISOIsKeptAsAFolder() async throws {
        let backup = #"""
            mkdir -p "$dest/BDMV" && printf x > "$dest/BDMV/index.bdmv"
            echo 'MSG:5036,260,1,"Copy complete. 1 titles saved.","Copy complete. %1 titles saved.","1"'
        """#
        let fake = try FakeMakeMKV(listing: listing(titles: [("0:00:10", 1)]), mkv: writesFile(saved), backup: backup)
        let job = await backupJob(fake, mode: .backupThenMkv)
        #expect(job.state == .succeeded, "\(job.errorMessage ?? "")")
        let backupFolder = try #require(job.producedFiles.first { JobRunner.isDirectory($0) })
        #expect(backupFolder.pathExtension.isEmpty, "\(backupFolder.lastPathComponent)")
        #expect(job.log.contains { $0.text.contains("instead of an ISO image") })
        #expect(fake.calls.contains("mkv file:"), "the MKV step must read the folder: \(fake.calls)")
    }

    @Test func folderWithoutADiscIsNotAcceptedAsAnISO() async throws {
        let backup = #"""
            mkdir -p "$dest/junk" && printf x > "$dest/junk/file"
            echo 'MSG:5036,260,1,"Copy complete. 1 titles saved.","Copy complete. %1 titles saved.","1"'
        """#
        let fake = try FakeMakeMKV(listing: listing(titles: [("0:00:10", 1)]), mkv: ":", backup: backup)
        let job = await backupJob(fake, mode: .backupDecrypted)
        #expect(job.state == .failed)
        #expect(job.errorMessage?.contains("Backup failed the check") == true, "\(job.errorMessage ?? "")")
        // Nothing counts as produced, but the failed backup is kept, and the logs go with it.
        #expect(job.producedFiles.isEmpty)
        let out = try #require(job.outputDirectory)
        let items = JobRunner.visibleItems(out)
        #expect(items.contains("INCOMPLETE.txt") && items.contains("bromelia-log.txt") && items.contains("makemkv-log.txt"), "\(items)")
    }

    @Test func archiveEverythingPreset() {
        var d = DriveConfig()
        d.name = "Left"
        d.rip.titleSelection.strategy = .longest
        d.applyArchiveEverything()
        #expect(d.name == "Left")
        #expect(d.rip.mode == .backupThenMkv && d.rip.keepBackupAfterMKV && d.rip.backupFormat == .folder)
        #expect(d.rip.titleSelection.strategy == .all)
        #expect(d.profile.mode == .generated && d.profile.generated.selectionRule == "+sel:all")
        #expect(d.archive.verifyRips && d.archive.checksums && d.rip.writeDiscInfoJSON)
    }

    @Test func settingsHaveSafeDefaults() throws {
        let c = try ConfigStore.decoder().decode(AppConfig.self, from: Data("{}".utf8))
        #expect(c.stallTimeoutMinutes == 30)
        #expect(c.preventSleep)
    }

    @Test func backgroundStepsKeepTheMacAwake() async throws {
        let model = AppModel(persistent: false)
        var switches: [Bool] = []
        model.keepAwakeSwitch = { switches.append($0) }
        var step = PostProcessStep()
        step.executable = "/bin/sh"
        step.arguments = "-c 'sleep 0.3'"
        step.runOn = .always
        step.background = true
        let context = PostProcessor.Context(status: .succeeded, values: [:], outputDirectory: root, files: [], manifestPath: "", environment: [:])
        model.background.enqueue(BackgroundWork(jobId: UUID(), title: "awake", steps: [step], context: context,
                                                logFile: root.appendingPathComponent("background.log")))
        #expect(switches == [true])
        model.config.preventSleep = false
        #expect(switches == [true, false], "turning the setting off releases it at once")
        model.config.preventSleep = true
        #expect(switches == [true, false, true])
        for _ in 0..<100 where model.background.isBusy { try await Task.sleep(nanoseconds: 100_000_000) }
        #expect(!model.background.isBusy)
        #expect(switches == [true, false, true, false], "released when the last step has finished")
    }

    private func writeUnfinished(_ dir: URL, pid: Int32, instance: String, jobs: String) throws {
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        try Data("{\"pid\": \(pid), \"instance\": \"\(instance)\", \"jobs\": [\(jobs)]}".utf8)
            .write(to: dir.appendingPathComponent("unfinished-\(pid).json"))
    }

    private func entry(_ id: String, _ state: String, output: URL? = nil) -> String {
        let out = output.map { ", \"outputDirectory\": \"\($0.path)\"" } ?? ""
        return #"{"id": "\#(id)", "title": "T", "driveName": "", "discName": "", "mode": "mkv", "state": "\#(state)", "logPath": ""\#(out)}"#
    }

    @Test func unfinishedJobsAreRecordedAfterAStop() throws {
        let fm = FileManager.default
        let data = root.appendingPathComponent("data", isDirectory: true)
        let saved = Paths.dataOverride
        Paths.dataOverride = data
        defer { Paths.dataOverride = saved }
        let movie = root.appendingPathComponent("Movie", isDirectory: true)
        let stage = movie.appendingPathComponent(".bromelia-incomplete-aaaa1111", isDirectory: true)
        try fm.createDirectory(at: stage, withIntermediateDirectories: true)
        try Data("partial".utf8).write(to: stage.appendingPathComponent("title_t00.mkv"))
        let empty = root.appendingPathComponent("Empty", isDirectory: true)
        let emptyStage = empty.appendingPathComponent(".bromelia-incomplete-bbbb2222", isDirectory: true)
        try fm.createDirectory(at: emptyStage, withIntermediateDirectories: true)
        try Data("tmp".utf8).write(to: emptyStage.appendingPathComponent(".tmp-split"))

        let gone = Process()
        gone.executableURL = URL(fileURLWithPath: "/usr/bin/true")
        try gone.run()
        gone.waitUntilExit()
        let running = "AAAA1111-0000-4000-8000-000000000001", nothing = "BBBB2222-0000-4000-8000-000000000002"
        let queued = "CCCC3333-0000-4000-8000-000000000003", other = "DDDD4444-0000-4000-8000-000000000004"
        try writeUnfinished(data, pid: gone.processIdentifier, instance: "gone",
                            jobs: [entry(running, "running", output: movie), entry(nothing, "running", output: empty), entry(queued, "queued")].joined(separator: ","))
        // Another Bromelia that is still running: left alone.
        try writeUnfinished(data, pid: getppid(), instance: "other", jobs: entry(other, "running"))

        let model = AppModel(persistent: true)
        let kept = movie.appendingPathComponent("INCOMPLETE - aaaa1111", isDirectory: true)
        let r1 = try #require(model.history.first { $0.id.uuidString == running })
        #expect(r1.state == .failed && r1.errorMessage?.hasPrefix("Interrupted") == true)
        #expect(r1.outputDirectory == kept.path)
        #expect(fm.fileExists(atPath: kept.appendingPathComponent("title_t00.mkv").path))
        let note = try String(contentsOf: kept.appendingPathComponent("INCOMPLETE.txt"), encoding: .utf8)
        #expect(note.contains("NOT a finished archive"))
        #expect(!fm.fileExists(atPath: stage.path))
        let r2 = try #require(model.history.first { $0.id.uuidString == nothing })
        #expect(r2.outputDirectory == nil)
        #expect(!fm.fileExists(atPath: empty.path), "nothing was saved: its folders are removed")
        let r3 = try #require(model.history.first { $0.id.uuidString == queued })
        #expect(r3.state == .cancelled && r3.errorMessage?.hasPrefix("Not started") == true)
        #expect(!model.history.contains { $0.id.uuidString == other })
        #expect(fm.fileExists(atPath: data.appendingPathComponent("unfinished-\(getppid()).json").path))
        #expect(model.lastError?.contains("3 unfinished job(s)") == true)

        // This process's own jobs are written down while they wait, and taken out once finished.
        let job = RipJob(source: .iso(path: "/nonexistent/test.iso"), drive: DriveConfig(), laneKey: "iso:unfinished", sourceLabel: "test", discLabel: "", mode: .mkv)
        job.state = .waiting
        job.startAt = Date().addingTimeInterval(3600)
        model.enqueue(job)
        let mine = UnfinishedJobs.file()
        let text = try String(contentsOf: mine, encoding: .utf8)
        #expect(text.contains(job.id.uuidString) && text.contains("\"waiting\""))
        model.cancel(job)
        #expect(!fm.fileExists(atPath: mine.path))
        try? fm.removeItem(at: data.appendingPathComponent("unfinished-\(getppid()).json"))
    }
}

// MARK: - MakeMKV notices, beta key, single files, one-pass rips

@Suite("MakeMKV parity")
struct ParityTests {
    private func message(_ line: String) -> RobotMessage {
        guard case .message(let m)? = RobotParser.parse(line: line) else { fatalError("not a message: \(line)") }
        return m
    }

    @Test func recognisesNotices() {
        #expect(MakeMKVNotice(message(#"MSG:1011,0,1,"Using LibreDrive mode (v06.3 id=4FBA32AEC678)","%1","Using LibreDrive mode (v06.3 id=4FBA32AEC678)""#))
                == .libreDrive("v06.3 id=4FBA32AEC678"))
        #expect(MakeMKVNotice(message(#"MSG:5055,0,0,"Evaluation period has expired, shareware functionality unavailable.","Evaluation period has expired, shareware functionality unavailable.""#)) == .keyExpired)
        #expect(MakeMKVNotice(message(#"MSG:5052,516,0,"Evaluation period has expired. Please purchase an activation key if you've found this application useful. You may still use all free functionality without any restrictions.","x""#)) == .keyExpired)
        #expect(MakeMKVNotice(message(#"MSG:5021,260,1,"This application version is too old.  Please download the latest version at http://www.makemkv.com/ or enter a registration key to continue using the current version.","x","http://www.makemkv.com/""#)) == .versionTooOld)
        #expect(MakeMKVNotice(message(#"MSG:2024,0,0,"LibreDrive compatible drive is required to open this disc - video can't be decrypted.","x""#)) == .libreDriveRequired)
        #expect(MakeMKVNotice(message(#"MSG:3007,0,0,"Using direct disc access mode","Using direct disc access mode""#)) == nil)
        #expect(MakeMKVNotice.keyExpired.isLicenseProblem && !MakeMKVNotice.libreDriveRequired.isLicenseProblem)
    }

    @Test func readsTheBetaKeyFromTheForumPage() {
        let key = "T-" + String(repeating: "aB3@_x", count: 11)
        let html = ##"<div class="codebox"><p>Code: <a href="#">Select all</a></p><pre><code>"## + key + ##"</code></pre></div> and is valid until end of October"##
        #expect(BetaKey.parse(html: html) == key)
        #expect(BetaKey.parse(html: "no key here") == nil)
    }

    @Test(arguments: [
        ("/Rips/Disc/VIDEO_TS/VTS_01_1.VOB", false, "file:/Rips/Disc"),
        ("/Rips/Disc/VIDEO_TS/VIDEO_TS.IFO", false, "file:/Rips/Disc"),
        ("/Rips/Disc/BDMV/PLAYLIST/00800.mpls", false, "file:/Rips/Disc"),
        ("/Rips/Disc/BDMV/STREAM/00001.m2ts", false, "file:/Rips/Disc"),
        ("/Rips/Disc/BDMV", true, "file:/Rips/Disc"),
        ("/Rips/Disc", true, "file:/Rips/Disc"),
        ("/Rips/Movie.ISO", false, "iso:/Rips/Movie.ISO"),
        ("/Rips/loose.m2ts", false, "file:/Rips/loose.m2ts"),
    ])
    func opensTheDiscAFileBelongsTo(path: String, isDirectory: Bool, expected: String) {
        #expect(SourceResolver.source(for: URL(fileURLWithPath: path), isDirectory: isDirectory).infoArgument == expected)
    }

    private func titles(_ durations: [Int]) -> DiscInfo {
        var d = DiscInfo()
        d.titles = durations.enumerated().map { i, s in
            var t = TitleInfo(index: i)
            t.attributes[AttributeID.duration.rawValue] = TitleInfo.formatDuration(s)
            t.attributes[AttributeID.originalTitleId.rawValue] = String(i + 1)
            return t
        }
        return d
    }

    @Test func onePassNeedsTheLongestTitlesAndEnoughOfThem() {
        let info = titles([600, 30, 900, 1200, 40])
        #expect(OnePass.minimumLength(chosen: [0, 2, 3], of: info, current: nil) == 41)
        #expect(OnePass.minimumLength(chosen: [0, 2], of: info, current: nil) == nil)          // too few to gain
        #expect(OnePass.minimumLength(chosen: [0, 1, 2, 3, 4], of: info, current: nil) == nil) // every title: "all" anyway
        #expect(OnePass.minimumLength(chosen: [1, 2, 3], of: info, current: nil) == nil)       // a longer title left out
        #expect(OnePass.minimumLength(chosen: [0, 2, 3], of: titles([600, 599, 900, 1200]), current: nil) == nil) // too close
        #expect(OnePass.minimumLength(chosen: [0, 2, 3], of: info, current: 120) == nil)       // already filtered
        let filtered = titles([600, 900, 1200].map { $0 })
        var shifted = filtered
        for i in shifted.titles.indices { shifted.titles[i].attributes[AttributeID.originalTitleId.rawValue] = ["1", "3", "4"][i] }
        #expect(OnePass.matches(shifted, chosen: [0, 2, 3], of: info))
        #expect(!OnePass.matches(titles([600, 900]), chosen: [0, 2, 3], of: info))
    }
}

@Suite("One-pass rips and notices in jobs", .serialized)
@MainActor
struct OnePassJobTests {
    let root: URL

    init() throws {
        Paths.dataOverride = FileManager.default.temporaryDirectory.appendingPathComponent("bromelia-test-data", isDirectory: true)
        root = FileManager.default.temporaryDirectory.appendingPathComponent("bromelia-1p-\(UUID().uuidString.prefix(8))", isDirectory: true)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
    }

    /// Titles (duration, source id) of the full listing; with `--minlength=N` the fake lists only titles of at
    /// least N seconds, renumbered, like makemkvcon.
    private func fake(_ all: [(String, Int)], extraInfo: String = "") throws -> FakeMakeMKV {
        let full = listing(titles: all)
        let dir = FileManager.default.temporaryDirectory.appendingPathComponent("bromelia-1pl-\(UUID().uuidString.prefix(8))", isDirectory: true)
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        // One listing per minimum length a test may ask for.
        for n in [0, 11, 21, 6] {
            let kept = all.filter { TitleInfo.parseDuration($0.0) >= n }
            try listing(titles: kept).write(to: dir.appendingPathComponent("min\(n).txt"), atomically: true, encoding: .utf8)
        }
        let mkv = """
            if [ "$title" = "all" ]; then
              for t in 0 1 2 3 4 5; do grep -q "title_t0$t.mkv" "$LISTING" && printf 'mkv data' > "$dest/title_t0$t.mkv"; done
            else
              printf 'mkv data' > "$dest/title_t0$title.mkv"
            fi
            echo 'MSG:5036,260,1,"Copy complete. 1 titles saved.","Copy complete. %1 titles saved.","1"'
        """
        let f = try FakeMakeMKV(listing: extraInfo + full, mkv: mkv)
        // Replace the info branch: pick the listing matching --minlength.
        var script = try String(contentsOf: f.executable, encoding: .utf8)
        script = script.replacingOccurrences(of: "echo \"$@\" >>", with: """
            MIN=0; for a in "$@"; do case "$a" in --minlength=*) MIN="${a#--minlength=}";; esac; done
            LISTING='\(dir.path)'/min$MIN.txt; [ -f "$LISTING" ] || LISTING='\(f.dir.path)/listing.txt'
            [ "$MIN" = 0 ] && LISTING='\(f.dir.path)/listing.txt'
            echo "$@" >>
            """)
        script = script.replacingOccurrences(of: "info) cat '\(f.dir.path)/listing.txt' ;;", with: "info) cat \"$LISTING\" ;;")
        try script.write(to: f.executable, atomically: true, encoding: .utf8)
        return f
    }

    private func run(_ f: FakeMakeMKV, configure: (inout DriveConfig) -> Void) async -> RipJob {
        var config = AppConfig()
        config.outputRoot = root.path
        var drive = DriveConfig()
        drive.automation.notify = false
        configure(&drive)
        let job = RipJob(source: .iso(path: "/nonexistent/test.iso"), drive: drive, laneKey: "iso:1p", sourceLabel: "test", discLabel: "", mode: .mkv)
        await JobRunner(job: job, config: config, makemkvcon: f.executable, mkvmerge: nil).run()
        return job
    }

    @Test func longestTitlesAreRippedInOnePass() async throws {
        let f = try fake([("0:00:30", 1), ("0:00:05", 2), ("0:00:20", 3), ("0:00:40", 4), ("0:00:10", 5)])
        let job = await run(f) {
            $0.rip.titleSelection.strategy = .longest
            $0.rip.titleSelection.longestCount = 3
        }
        #expect(job.state == .succeeded, "\(job.errorMessage ?? "") \(f.calls)")
        let mkvCalls = f.calls.split(separator: "\n").filter { $0.contains(" mkv ") }
        #expect(mkvCalls.count == 1, "\(f.calls)")
        #expect(mkvCalls.first?.contains("--minlength=11") == true, "\(mkvCalls)")
        #expect(mkvCalls.first?.contains("mkv iso:/nonexistent/test.iso all ") == true, "\(mkvCalls)")
        #expect(job.producedFiles.count == 3)
        #expect(job.ripInfo?.titles.map(\.sourceTitleId) == [1, 3, 4])
        #expect(job.log.contains { $0.text.contains("in one pass") })
    }

    @Test func otherSelectionsAreRippedTitleByTitle() async throws {
        let f = try fake([("0:00:30", 1), ("0:00:05", 2), ("0:00:20", 3), ("0:00:40", 4), ("0:00:10", 5)])
        let job = await run(f) {
            $0.rip.titleSelection.strategy = .indices
            $0.rip.titleSelection.indexPattern = "0,1,3"   // leaves out a longer title (20 s)
        }
        #expect(job.state == .succeeded, "\(job.errorMessage ?? "")")
        #expect(f.calls.split(separator: "\n").filter { $0.contains(" mkv ") }.count == 3)
        #expect(!f.calls.contains("--minlength"))
    }

    @Test func expiredKeyExplainsTheFailure() async throws {
        let expired = #"MSG:5055,0,0,"Evaluation period has expired, shareware functionality unavailable.","Evaluation period has expired, shareware functionality unavailable.""#
        let f = try FakeMakeMKV(listing: expired + "\n" + #"MSG:5010,0,0,"Failed to open disc","Failed to open disc""# + "\n", mkv: ":")
        let job = await run(f) { _ in }
        #expect(job.state == .failed)
        #expect(job.makemkvProblem == .keyExpired)
        #expect(job.errorMessage?.contains("key has expired") == true, "\(job.errorMessage ?? "")")
    }

    @Test func libreDriveIsRecorded() async throws {
        let libre = #"MSG:1011,0,1,"Using LibreDrive mode (v06.3 id=4FBA32AEC678)","%1","Using LibreDrive mode (v06.3 id=4FBA32AEC678)""# + "\n"
        let f = try fake([("0:00:30", 1)], extraInfo: libre)
        let job = await run(f) { _ in }
        #expect(job.state == .succeeded, "\(job.errorMessage ?? "")")
        #expect(job.libreDrive == "v06.3 id=4FBA32AEC678")
        let record = try JSONSerialization.jsonObject(with: Data(contentsOf: job.outputDirectory!.appendingPathComponent("bromelia.json"))) as? [String: Any]
        #expect(record?["libreDrive"] as? String == "v06.3 id=4FBA32AEC678")
    }
}

// MARK: - Features of other ripping tools

import Network

/// Whether TCP connections to 127.0.0.1 work here (some firewalls block them; the network tests are skipped then).
private enum Loopback {
    static let works: Bool = {
        let server = socket(AF_INET, SOCK_STREAM, 0)
        guard server >= 0 else { return false }
        defer { close(server) }
        var addr = sockaddr_in()
        addr.sin_family = sa_family_t(AF_INET)
        addr.sin_addr.s_addr = inet_addr("127.0.0.1")
        addr.sin_port = 0
        var len = socklen_t(MemoryLayout<sockaddr_in>.size)
        let bound = withUnsafeMutablePointer(to: &addr) { $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { bind(server, $0, len) == 0 && listen(server, 1) == 0 && getsockname(server, $0, &len) == 0 } }
        guard bound else { return false }
        let client = socket(AF_INET, SOCK_STREAM, 0)
        defer { close(client) }
        _ = fcntl(client, F_SETFL, O_NONBLOCK)
        _ = withUnsafePointer(to: &addr) { $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { connect(client, $0, len) } }
        var fds = pollfd(fd: client, events: Int16(POLLOUT), revents: 0)
        guard poll(&fds, 1, 2000) == 1 else { return false }
        var err: Int32 = 0
        var errLen = socklen_t(MemoryLayout<Int32>.size)
        getsockopt(client, SOL_SOCKET, SO_ERROR, &err, &errLen)
        return err == 0
    }()
}

/// Accepts one HTTP request on 127.0.0.1 and hands back its raw bytes.
private final class OneShotHTTPServer: @unchecked Sendable {
    let listener: NWListener
    private let lock = NSLock()
    private var received = Data()
    private var done = false

    init() throws {
        listener = try NWListener(using: .tcp, on: .any)
        listener.newConnectionHandler = { [self] c in
            c.start(queue: .global())
            read(c)
        }
        listener.start(queue: .global())
    }

    private func read(_ c: NWConnection) {
        c.receive(minimumIncompleteLength: 1, maximumLength: 65536) { [self] data, _, fin, _ in
            lock.lock()
            if let data { received.append(data) }
            let text = String(decoding: received, as: UTF8.self)
            var complete = false
            if let head = text.range(of: "\r\n\r\n") {
                let length = text[..<head.lowerBound].components(separatedBy: "\r\n")
                    .first { $0.lowercased().hasPrefix("content-length:") }.flatMap { Int($0.split(separator: ":")[1].trimmingCharacters(in: .whitespaces)) } ?? 0
                complete = text[head.upperBound...].utf8.count >= length
            }
            if complete { done = true }
            lock.unlock()
            if complete || fin {
                c.send(content: Data("HTTP/1.1 204 No Content\r\nContent-Length: 0\r\nConnection: close\r\n\r\n".utf8), completion: .contentProcessed { _ in c.cancel() })
            } else {
                read(c)
            }
        }
    }

    var port: UInt16 { listener.port?.rawValue ?? 0 }
    var request: String? { lock.lock(); defer { lock.unlock() }; return done ? String(decoding: received, as: UTF8.self) : nil }

    func waitForPort() async {
        for _ in 0..<100 where port == 0 { try? await Task.sleep(nanoseconds: 20_000_000) }
    }
}

@Suite("Integrations")
struct IntegrationFeatureTests {
    @Test func notificationDeliveries() throws {
        func body(_ d: NotificationSender.Delivery?) -> [String: Any]? {
            guard case let .http(_, _, data)? = d else { return nil }
            return try? JSONSerialization.jsonObject(with: data) as? [String: Any]
        }
        let discord = NotificationSender.delivery(for: "https://discord.com/api/webhooks/1/abc", title: "T", body: "B", status: "success")
        #expect(body(discord)?["content"] as? String == "**T**\nB")
        let slack = NotificationSender.delivery(for: "https://hooks.slack.com/services/x/y/z", title: "T", body: "B", status: "failed")
        #expect(body(slack)?["text"] as? String == "*T*\nB")
        guard case let .http(url, headers, data)? = NotificationSender.delivery(for: "ntfy://rips", title: "T", body: "B", status: "failed") else {
            Issue.record("ntfy"); return
        }
        #expect(url.absoluteString == "https://ntfy.sh/rips" && headers["Title"] == "T" && String(decoding: data, as: UTF8.self) == "B")
        guard case let .http(url2, _, _)? = NotificationSender.delivery(for: "ntfys://ntfy.example.org/rips", title: "T", body: "B", status: "success") else {
            Issue.record("ntfys"); return
        }
        #expect(url2.absoluteString == "https://ntfy.example.org/rips")
        #expect(body(NotificationSender.delivery(for: "https://example.org/hook", title: "T", body: "B", status: "errors"))?["status"] as? String == "errors")
        #expect(NotificationSender.delivery(for: "tgram://bot/chat", title: "T", body: "B", status: "success") == .apprise(url: "tgram://bot/chat"))
        #expect(NotificationSender.delivery(for: "", title: "T", body: "B", status: "success") == nil)
    }

    @Test(.enabled(if: Loopback.works, "connections to 127.0.0.1 are blocked here"))
    func notificationReachesAWebhook() async throws {
        let server = try OneShotHTTPServer()
        await server.waitForPort()
        var target = NotificationTarget()
        target.url = "http://127.0.0.1:\(server.port)/hook"
        var skipped = NotificationTarget()
        skipped.url = "http://127.0.0.1:1/never"
        skipped.onlyProblems = true
        let log = LineCollector()
        await NotificationSender.send([target, skipped], title: "Rip finished: Inception", body: "2 item(s)", status: "success") { log.append($0) }
        for _ in 0..<100 where server.request == nil { try await Task.sleep(nanoseconds: 20_000_000) }
        let request = try #require(server.request)
        #expect(request.hasPrefix("POST /hook HTTP/1.1"))
        #expect(request.contains(#""title":"Rip finished: Inception""#))
        #expect(log.all.isEmpty, "\(log.all)")
        server.listener.cancel()
    }

    @Test func metadataResults() throws {
        let tmdb = #"{"results":[{"id":1,"title":"Inception Behind","release_date":"2011-01-01"},{"id":27205,"title":"Inception","release_date":"2010-07-15"}]}"#
        let m = try #require(MetadataLookup.parse(Data(tmdb.utf8), provider: .tmdb, name: "INCEPTION"))
        #expect(m.title == "Inception" && m.year == 2010 && m.tmdbId == 27205)
        let tv = #"{"results":[{"id":37854,"name":"One Piece","first_air_date":"1999-10-20"}]}"#
        #expect(MetadataLookup.parse(Data(tv.utf8), provider: .tmdb, name: "One Piece")?.year == 1999)
        let omdb = #"{"Title":"Friends","Year":"1994–2004","imdbID":"tt0108778","Response":"True"}"#
        let o = try #require(MetadataLookup.parse(Data(omdb.utf8), provider: .omdb, name: "Friends"))
        #expect(o.year == 1994 && o.imdbId == "tt0108778")
        #expect(MetadataLookup.parse(Data(#"{"Response":"False"}"#.utf8), provider: .omdb, name: "x") == nil)
        var c = MetadataConfig()
        c.provider = .tmdb
        c.apiKey = "0123456789abcdef0123456789abcdef"
        let r = try #require(MetadataLookup.request(name: "One Piece", kind: .tv, config: c))
        #expect(r.url?.path == "/3/search/tv" && r.url?.query?.contains("api_key=0123") == true)
        c.apiKey = "eyJ" + String(repeating: "x", count: 60)
        let bearer = try #require(MetadataLookup.request(name: "One Piece", kind: .movie, config: c))
        #expect(bearer.value(forHTTPHeaderField: "Authorization")?.hasPrefix("Bearer eyJ") == true && bearer.url?.query?.contains("api_key") == false)
    }

    @Test func otherDiscModesAndBetaKeys() {
        var d = DriveConfig()
        d.rip.mode = .backupThenMkv
        #expect(DiscContent.mode(flags: [.blurayFiles], content: { .data }, drive: d) == .backupThenMkv)
        #expect(DiscContent.mode(flags: [], content: { .video }, drive: d) == .backupThenMkv)
        #expect(DiscContent.mode(flags: [], content: { .audio }, drive: d) == .audioCD)
        #expect(DiscContent.mode(flags: [], content: { .data }, drive: d) == .dataImage)
        d.other.ripAudioCDs = false
        d.other.imageDataDiscs = false
        #expect(DiscContent.mode(flags: [], content: { .audio }, drive: d) == nil)
        #expect(DiscContent.mode(flags: [], content: { .data }, drive: d) == nil)
        #expect(BetaKey.mayReplace(nil) && BetaKey.mayReplace("") && BetaKey.mayReplace("T-abc"))
        #expect(!BetaKey.mayReplace("M-purchasedkey"))
        d.rip.formatModes = ["bluray": .backup]
        #expect(d.rip.mode(for: .bluray) == .backup && d.rip.mode(for: .dvd) == .backupThenMkv && d.rip.mode(for: nil) == .backupThenMkv)
    }

    @Test func drutilListIsMatchedByModel() {
        let lines = ["   Vendor   Product           Rev   Bus       SupportLevel",
                     "1  HL-DT-ST BD-RE  WH16NS60  1.02  USB       Unsupported",
                     "2  PIONEER  BD-RW   BDR-XS07 1.04  USB       Unsupported"]
        #expect(DriveControl.drutilIndices(lines, matching: "BD-RE HL-DT-ST BD-RE  WH16NS60 1.02 KLAM6E84325") == [1])
        #expect(DriveControl.drutilIndices(lines, matching: "") == [1, 2])
    }

    @Test func webRequestsAndAccess() throws {
        let raw = "GET /api/status?token=s3cret HTTP/1.1\r\nHost: localhost:51280\r\nX-Bromelia: 1\r\n\r\n"
        let r = try #require(HTTPRequest.parse(Data(raw.utf8)))
        #expect(r.method == "GET" && r.path == "/api/status" && r.query["token"] == "s3cret" && r.headers["host"] == "localhost:51280")
        #expect(HTTPRequest.parse(Data("GET / HTTP/1.1\r\nHost: x".utf8)) == nil)
        var c = WebUIConfig()
        #expect(WebAccess.allowed(r, config: c).0)
        var evil = r
        evil.headers["host"] = "attacker.example:51280"
        #expect(WebAccess.allowed(evil, config: c).1 == 403)
        var post = r
        post.method = "POST"
        post.headers["x-bromelia"] = nil
        #expect(WebAccess.allowed(post, config: c).1 == 403)
        c.token = "s3cret"
        #expect(WebAccess.allowed(evil, config: c).0)
        var noToken = r
        noToken.query = [:]
        #expect(WebAccess.allowed(noToken, config: c).1 == 401)
        noToken.headers["authorization"] = "Bearer s3cret"
        #expect(WebAccess.allowed(noToken, config: c).0)
        var open = WebUIConfig()
        open.address = "0.0.0.0"
        #expect(WebAccess.startProblem(open) != nil)
        open.token = "t"
        #expect(WebAccess.startProblem(open) == nil)
    }
}

@Suite("Integrations in jobs", .serialized)
@MainActor
struct IntegrationJobTests {
    let root: URL

    init() throws {
        Paths.dataOverride = FileManager.default.temporaryDirectory.appendingPathComponent("bromelia-test-data", isDirectory: true)
        root = FileManager.default.temporaryDirectory.appendingPathComponent("bromelia-int-\(UUID().uuidString.prefix(8))", isDirectory: true)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
    }

    private func run(_ fake: FakeMakeMKV, source: DiscSource = .iso(path: "/nonexistent/test.iso"), mode: RipMode = .mkv,
                     configure: (inout DriveConfig, inout AppConfig) -> Void = { _, _ in }, prepare: (RipJob) -> Void = { _ in }) async -> RipJob {
        var config = AppConfig()
        config.outputRoot = root.path
        var drive = DriveConfig()
        drive.automation.notify = false
        drive.automation.ejectWhenDone = false
        configure(&drive, &config)
        let job = RipJob(source: source, drive: drive, laneKey: "lane", sourceLabel: "test", discLabel: "", mode: mode)
        prepare(job)
        await JobRunner(job: job, config: config, makemkvcon: fake.executable, mkvmerge: nil).run()
        return job
    }

    private func files(_ dir: URL) -> [String] {
        let base = dir.resolvingSymlinksInPath().path
        let e = FileManager.default.enumerator(at: dir, includingPropertiesForKeys: nil, options: [.skipsHiddenFiles])
        var out: [String] = []
        while let u = e?.nextObject() as? URL {
            let p = u.resolvingSymlinksInPath().path
            if !JobRunner.isDirectory(u) && p.hasPrefix(base + "/") { out.append(String(p.dropFirst(base.count + 1))) }
        }
        return out.sorted()
    }

    @Test func moviesAreNamedForMediaServers() async throws {
        let fake = try FakeMakeMKV(listing: listing(titles: [("1:50:00", 1), ("0:05:00", 2)]), mkv: writesFile(saved))
        let job = await run(fake) { d, _ in d.output.layout = .mediaServer }
        #expect(job.state == .succeeded, "\(job.errorMessage ?? "")")
        #expect(files(root).filter { $0.hasSuffix(".mkv") } == ["Movies/Sample Movie/Other/Sample Movie - Playlist 00002.mkv",
                                                                   "Movies/Sample Movie/Sample Movie.mkv"], "\(files(root))")
    }

    @Test func episodesAreNamedForMediaServers() async throws {
        let fake = try FakeMakeMKV(listing: listing(volume: "SAMPLE_SHOW_S2_D1", titles: [("0:22:00", 1), ("0:22:30", 2), ("0:21:40", 3)]),
                                   mkv: writesFile(saved))
        let job = await run(fake) { d, _ in d.output.layout = .mediaServer }
        #expect(job.state == .succeeded, "\(job.errorMessage ?? "")")
        #expect(files(root).filter { $0.hasSuffix(".mkv") } == (1...3).map { "TV Shows/Sample Show/Season 02/Sample Show - S02E0\($0).mkv" }, "\(files(root))")

        // The next disc of the season is added to the same show and Season folders.
        let fake2 = try FakeMakeMKV(listing: listing(volume: "SAMPLE_SHOW_S2_D2", titles: [("0:22:00", 1), ("0:22:30", 2), ("0:21:40", 3)]),
                                    mkv: writesFile(saved))
        let job2 = await run(fake2, configure: { d, _ in d.output.layout = .mediaServer }, prepare: { $0.firstEpisode = 4 })
        #expect(job2.state == .succeeded, "\(job2.errorMessage ?? "")")
        #expect(files(root).filter { $0.hasSuffix(".mkv") } == (1...6).map { "TV Shows/Sample Show/Season 02/Sample Show - S02E0\($0).mkv" }, "\(files(root))")
        #expect(job2.producedFiles.allSatisfy { FileManager.default.fileExists(atPath: $0.path) })
        #expect(try Checksums.verify(folder: root.appendingPathComponent("TV Shows/Sample Show")).isEmpty)
    }

    @Test func backgroundStepsRunAfterTheJob() async throws {
        let marker = root.appendingPathComponent("encoded.txt")
        let fake = try FakeMakeMKV(listing: listing(titles: [("0:30:00", 1)]), mkv: writesFile(saved))
        let job = await run(fake) { d, _ in
            var step = PostProcessStep()
            step.name = "Encode"
            step.executable = "/bin/sh"
            step.arguments = "-c 'sleep 0.3; echo \"$BROMELIA_STATUS\" > \"$0\"' \(marker.path)"
            step.background = true
            d.postProcess = [step]
        }
        #expect(job.state == .succeeded)
        #expect(!FileManager.default.fileExists(atPath: marker.path), "a background step must not run during the job")
        let work = try #require(job.background)
        let queue = BackgroundQueue()
        queue.enqueue(work)
        for _ in 0..<200 where queue.isBusy { try await Task.sleep(nanoseconds: 20_000_000) }
        #expect(queue.items.first?.state == .done, "\(queue.items.first?.message ?? "")")
        #expect(try String(contentsOf: marker, encoding: .utf8) == "success\n")
    }

    @Test func formatModesReplaceTheConfiguredMode() async throws {
        let fake = try FakeMakeMKV(listing: listing(titles: [("0:30:00", 1)]), mkv: writesFile(saved))
        let job = await run(fake, configure: { d, _ in d.rip.formatModes = ["bluray": .infoOnly] }, prepare: { $0.usesConfiguredMode = true })
        #expect(job.state == .succeeded, "\(job.errorMessage ?? "")")
        #expect(job.mode == .infoOnly)
        #expect(job.producedFiles.map(\.lastPathComponent) == ["disc-info.json"])
        #expect(!fake.calls.contains(" mkv "))
    }

    @Test func audioCDsUseTheAudioCommand() async throws {
        let fake = try FakeMakeMKV(listing: "", mkv: ":")
        let job = await run(fake, source: .drive(index: 0, devicePath: "/dev/fake9"), mode: .audioCD) { d, _ in
            d.other.audioCommand = #"/bin/sh -c 'mkdir -p "Artist - Album" && printf "%s" "$0" > "Artist - Album/01 - Track.flac"' {device}"#
        }
        #expect(job.state == .succeeded, "\(job.errorMessage ?? "")")
        let flac = try #require(files(root).first { $0.hasSuffix(".flac") })
        #expect(flac.hasSuffix("Artist - Album/01 - Track.flac"), "\(files(root))")
        #expect(try String(contentsOf: root.appendingPathComponent(flac), encoding: .utf8) == "/dev/fake9")
        #expect(job.checksums.count == 1)
        #expect(!fake.calls.contains("info"), "makemkvcon isn't used for audio CDs")
    }

    @Test func missingAudioRipperFailsClearly() async throws {
        let fake = try FakeMakeMKV(listing: "", mkv: ":")
        let job = await run(fake, source: .drive(index: 0, devicePath: "/dev/fake9"), mode: .audioCD) { d, _ in
            d.other.audioCommand = "no-such-ripper-xyz {device}"
        }
        #expect(job.state == .failed)
        #expect(job.errorMessage?.contains("cyanrip or abcde") == true, "\(job.errorMessage ?? "")")
    }

    nonisolated static var dvdISO: String? { ProcessInfo.processInfo.environment["BROMELIA_TEST_DVD_ISO"] }

    /// Attaches a disc image as a device and images it like a data disc; the copy must be identical.
    @Test(.enabled(if: IntegrationJobTests.dvdISO != nil, "set BROMELIA_TEST_DVD_ISO to run"))
    func dataDiscsAreImagedExactly() async throws {
        let iso = try #require(Self.dvdISO)
        let attach = LineCollector()
        let hdiutil = URL(fileURLWithPath: "/usr/bin/hdiutil")
        let a = try await ProcessRunner(executable: hdiutil, arguments: ["attach", "-readonly", "-nomount", iso]).run { attach.append($0) }
        try #require(a.exitCode == 0)
        let device = try #require(attach.all.compactMap { $0.split(separator: " ").first.map(String.init) }.first { $0.hasPrefix("/dev/disk") })
        defer { Task { _ = try? await ProcessRunner(executable: hdiutil, arguments: ["detach", device]).run { _ in } } }
        let fake = try FakeMakeMKV(listing: "", mkv: ":")
        let job = await run(fake, source: .drive(index: 0, devicePath: device), mode: .dataImage)
        #expect(job.state == .succeeded, "\(job.errorMessage ?? "") \(job.log.suffix(10).map(\.text))")
        let image = try #require(job.producedFiles.first)
        #expect(image.pathExtension == "iso")
        let original = try Checksums.sha256(of: URL(fileURLWithPath: iso))
        #expect(job.checksums.first?.sha256 == original)
    }

    @Test(.enabled(if: Loopback.works, "connections to 127.0.0.1 are blocked here"))
    func webServerServesStatusAndActions() async throws {
        let model = AppModel(persistent: false)
        var web = WebUIConfig()
        web.enabled = true
        web.port = Int.random(in: 52000...58000)
        model.web.apply(web)
        try #require(model.web.lastError == nil, "\(model.web.lastError ?? "")")
        defer { model.web.stop() }
        try await Task.sleep(nanoseconds: 200_000_000)
        let base = "http://127.0.0.1:\(web.port)"
        let (status, r1) = try await URLSession.shared.data(from: URL(string: base + "/api/status")!)
        #expect((r1 as? HTTPURLResponse)?.statusCode == 200)
        let json = try JSONSerialization.jsonObject(with: status) as? [String: Any]
        #expect(json?["drives"] is [Any] && json?["jobs"] is [Any] && json?["background"] is [Any])
        let (page, r2) = try await URLSession.shared.data(from: URL(string: base + "/")!)
        #expect((r2 as? HTTPURLResponse)?.statusCode == 200 && String(decoding: page, as: UTF8.self).contains("<title>Bromelia</title>"))
        var post = URLRequest(url: URL(string: base + "/api/jobs/nope/cancel")!)
        post.httpMethod = "POST"
        let (_, r3) = try await URLSession.shared.data(for: post)
        #expect((r3 as? HTTPURLResponse)?.statusCode == 403, "actions need the X-Bromelia header")
        post.setValue("1", forHTTPHeaderField: "X-Bromelia")
        let (body, r4) = try await URLSession.shared.data(for: post)
        #expect((r4 as? HTTPURLResponse)?.statusCode == 400 && String(decoding: body, as: UTF8.self) == "No such job")
    }
}

// MARK: - Archives: fingerprints, already archived, verifying

@Suite("Archive checks and discs archived before", .serialized)
@MainActor
struct ArchiveCheckTests {
    let root: URL

    init() throws {
        Paths.dataOverride = FileManager.default.temporaryDirectory.appendingPathComponent("bromelia-test-data", isDirectory: true)
        root = FileManager.default.temporaryDirectory.appendingPathComponent("bromelia-arch-\(UUID().uuidString.prefix(8))", isDirectory: true)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
    }

    @Test func fingerprintsMatchEveryPlatform() throws {
        let dvd = info(try String(contentsOf: fixturesDir.appendingPathComponent("info-dvd.txt"), encoding: .utf8))
        let shared = try JSONSerialization.jsonObject(with: Data(contentsOf: fixturesDir.appendingPathComponent("fingerprints.json"))) as? [String: Any]
        #expect(DiscFingerprint.of(dvd) == shared?["info-dvd"] as? String)
        let a = info(listing(volume: "SHOW_S1_D1", titles: [("0:22:10", 1), ("0:22:40", 2), ("0:23:05", 3)]))
        let renumbered = info(listing(volume: "SHOW_S1_D1", titles: [("0:23:05", 3), ("0:22:10", 1), ("0:22:40", 2)]))
        let otherDisc = info(listing(volume: "SHOW_S1_D1", titles: [("0:22:12", 1), ("0:22:40", 2), ("0:23:05", 3)]))
        #expect(DiscFingerprint.of(a) == DiscFingerprint.of(renumbered), "MakeMKV's title numbers don't matter")
        #expect(DiscFingerprint.of(a) != DiscFingerprint.of(otherDisc), "another disc of the set, same label")
        #expect(DiscFingerprint.of(DiscInfo()) == nil)
    }

    private func writeRecord(_ dir: URL, _ name: String, status: String, fingerprint: String) throws {
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        let text = #"{"format": "bromelia-archive", "status": "\#(status)", "finishedAt": "2024-05-01T10:00:00Z", "disc": {"label": "X", "fingerprint": "\#(fingerprint)"}}"#
        try Data(text.utf8).write(to: dir.appendingPathComponent(name))
    }

    @Test func findsEarlierArchives() throws {
        let movie = root.appendingPathComponent("Movies/Movie (2001)", isDirectory: true)
        try writeRecord(movie, "bromelia (2).json", status: "success", fingerprint: "v1:aaaa")
        try writeRecord(root.appendingPathComponent("Other [READ ERRORS]"), "bromelia.json", status: "errors", fingerprint: "v1:bbbb")
        let when = Date(timeIntervalSince1970: 1_700_000_000)
        let candidates = [
            ArchivedCandidate(fingerprint: "v1:cccc", folder: root.appendingPathComponent("gone").path, state: .succeeded, finishedAt: when),
            ArchivedCandidate(fingerprint: "v1:dddd", folder: root.path, state: .completedWithErrors, finishedAt: when),
            ArchivedCandidate(fingerprint: "v1:eeee", folder: root.path, state: .succeeded, finishedAt: when),
        ]
        #expect(ArchiveLookup.find(fingerprint: "v1:eeee", candidates: candidates, root: nil) == ArchivedMatch(folder: root.path, archivedAt: when))
        #expect(ArchiveLookup.find(fingerprint: "v1:aaaa", candidates: candidates, root: root)
                == ArchivedMatch(folder: movie.path, archivedAt: Date(timeIntervalSince1970: 1_714_557_600)))
        #expect(ArchiveLookup.find(fingerprint: "v1:aaaa", candidates: candidates, root: nil) == nil)
        for fp in ["v1:bbbb", "v1:cccc", "v1:dddd"] {
            #expect(ArchiveLookup.find(fingerprint: fp, candidates: candidates, root: root) == nil, "\(fp)")
        }
        #expect(ArchiveLookup.find(fingerprint: nil, candidates: candidates, root: root) == nil)
    }

    /// An archive folder: files with a SHA256SUMS listing them.
    private func makeArchive(_ dir: URL, _ names: [String]) throws {
        var entries: [Checksums.Entry] = []
        for name in names {
            let url = dir.appendingPathComponent(name)
            try FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
            let data = Data("contents of \(name), long enough to hash".utf8)
            try data.write(to: url)
            entries.append(.init(path: name, size: Int64(data.count), sha256: try Checksums.sha256(of: url)))
        }
        try Checksums.render(entries).write(to: dir.appendingPathComponent(Checksums.fileName), atomically: true, encoding: .utf8)
    }

    @Test func verifiesArchives() throws {
        let good = root.appendingPathComponent("Good", isDirectory: true)
        let bad = root.appendingPathComponent("TV Shows/Bad", isDirectory: true)
        try makeArchive(good, ["Good.mkv", "Backup/BDMV/index.bdmv"])
        try makeArchive(bad, ["Season 01/E01.mkv", "Season 01/E02.mkv", "Season 01/E03.mkv"])
        try makeArchive(root.appendingPathComponent(".bromelia-incomplete-1234"), ["Good.mkv"])
        // One byte changed, one file gone, one file added; Bromelia's own files are not "extra".
        let e1 = bad.appendingPathComponent("Season 01/E01.mkv")
        var bytes = try Data(contentsOf: e1)
        bytes[3] ^= 1
        try bytes.write(to: e1)
        try FileManager.default.removeItem(at: bad.appendingPathComponent("Season 01/E02.mkv"))
        try Data("1\n00:00:01,000 --> 00:00:02,000\nHi\n".utf8).write(to: bad.appendingPathComponent("Season 01/E01.srt"))
        // .nfo files and the poster of a media server library aren't in SHA256SUMS, and aren't reported.
        try Data("<episodedetails/>".utf8).write(to: bad.appendingPathComponent("Season 01/E02.nfo"))
        try Data([0xFF, 0xD8]).write(to: bad.appendingPathComponent("poster.jpg"))
        try Data("{}".utf8).write(to: bad.appendingPathComponent("bromelia.json"))
        try Data("log".utf8).write(to: bad.appendingPathComponent("bromelia-log.txt"))
        try Data("log".utf8).write(to: bad.appendingPathComponent("makemkv-log.txt"))
        try Data("log".utf8).write(to: bad.appendingPathComponent("makemkv-debug-log-1.txt"))

        let folders = ArchiveVerifier.folders(under: root)
        #expect(folders.map(\.path) == [good.path, bad.path], "hidden (staging) folders are left out")
        let (results, stopped) = ArchiveVerifier.verify(folders: folders)
        #expect(!stopped && results.count == 2)
        let g = try #require(results.first { $0.folder == good.path })
        #expect(g.ok && g.files == 2 && g.extra.isEmpty)
        let b = try #require(results.first { $0.folder == bad.path })
        #expect(!b.ok)
        #expect(b.changed == ["Season 01/E01.mkv"])
        #expect(b.missing == ["Season 01/E02.mkv"])
        #expect(b.extra == ["Season 01/E01.srt"])
        #expect(b.summary == "1 changed, 1 missing of 3 file(s); 1 not listed")
        // Stopping keeps only folders that were finished.
        let none = ArchiveVerifier.verify(folders: folders) { _, _, _, _ in false }
        #expect(none.stopped && none.results.isEmpty)
        // When each folder was checked, kept across runs.
        let file = root.appendingPathComponent("checks/archive-checks.json")
        #expect(CheckRecords.load(from: file).isEmpty)
        let when = Date(timeIntervalSince1970: 1_700_000_000)
        CheckRecords.save(Dictionary(uniqueKeysWithValues: results.map { ($0.folder, CheckRecord(checkedAt: when, ok: $0.ok, summary: $0.summary)) }), to: file)
        let again = CheckRecords.load(from: file)
        #expect(again[bad.path] == CheckRecord(checkedAt: when, ok: false, summary: "1 changed, 1 missing of 3 file(s); 1 not listed"))
        #expect(again[good.path]?.ok == true)
    }

    @Test func verifiesFromTheApp() async throws {
        let good = root.appendingPathComponent("Good", isDirectory: true)
        try makeArchive(good, ["Good.mkv"])
        let model = AppModel(persistent: false)
        model.config.outputRoot = root.path
        let now = Date()
        #expect(!model.verifyDue(at: now), "archiveCheck is off by default")
        model.config.archiveCheck.intervalDays = 30
        #expect(model.verifyDue(at: now))
        #expect(model.startVerify(root))
        #expect(!model.startVerify(root), "one at a time")
        for _ in 0..<100 where model.verify.running { try await Task.sleep(nanoseconds: 50_000_000) }
        #expect(!model.verify.running && model.verify.results.count == 1)
        #expect(model.checkRecords[good.path]?.ok == true)
        #expect(model.checkRecords[model.outputRootURL.path] != nil, "the whole tree, for the schedule")
        #expect(!model.verifyDue(at: now.addingTimeInterval(86400)))
        #expect(model.verifyDue(at: now.addingTimeInterval(31 * 86400)))
        // A damaged file shows in the next run.
        try Data("bit rot".utf8).write(to: good.appendingPathComponent("Good.mkv"))
        #expect(model.webAction(kind: "verify", id: "", action: "start") == nil)
        for _ in 0..<100 where model.verify.running { try await Task.sleep(nanoseconds: 50_000_000) }
        #expect(model.checkRecords[good.path]?.ok == false)
        let status = model.webStatus()["verify"] as? [String: Any]
        #expect((status?["damaged"] as? [[String: Any]])?.count == 1)
    }

    private func archivedJob(_ fake: FakeMakeMKV, automatic: Bool, policy: AlreadyArchived,
                             candidates: [ArchivedCandidate] = [], root: URL? = nil) async -> RipJob {
        var config = AppConfig()
        config.outputRoot = (root ?? self.root).path
        var drive = DriveConfig()
        drive.automation.notify = false
        drive.automation.ejectWhenDone = false
        drive.automation.alreadyArchived = policy
        drive.automation.waitForMountSeconds = 0
        let job = RipJob(source: .iso(path: "/nonexistent/test.iso"), drive: drive, laneKey: "iso:archived", sourceLabel: "test", discLabel: "", mode: .mkv)
        job.isAutomatic = automatic
        job.archivedCandidates = candidates
        await JobRunner(job: job, config: config, makemkvcon: fake.executable, mkvmerge: nil).run()
        return job
    }

    @Test func discsArchivedBefore() async throws {
        let fake = try FakeMakeMKV(listing: listing(titles: [("0:30:00", 1)]), mkv: writesFile(saved))
        let first = await archivedJob(fake, automatic: false, policy: .skip)
        #expect(first.state == .succeeded, "\(first.errorMessage ?? "")")
        let fp = try #require(first.fingerprint)
        let folder = try #require(first.outputDirectory)
        let record = try String(contentsOf: folder.appendingPathComponent("bromelia.json"), encoding: .utf8)
        #expect(record.contains(fp), "the fingerprint is in the archive record")

        // Automatic rips of the same disc: found through the archive record under the output folder.
        let skipped = await archivedJob(fake, automatic: true, policy: .skip)
        #expect(skipped.state == .cancelled)
        #expect(skipped.errorMessage?.hasPrefix("Already archived in ") == true && skipped.errorMessage?.contains(folder.path) == true,
                "\(skipped.errorMessage ?? "")")
        #expect(skipped.outputDirectory == nil)
        let asked = await archivedJob(fake, automatic: true, policy: .ask)
        #expect(asked.state == .cancelled && asked.errorMessage?.contains("Rip it again from the drive page") == true)
        #expect(await archivedJob(fake, automatic: true, policy: .ripAgain).state == .succeeded)
        #expect(await archivedJob(fake, automatic: false, policy: .skip).state == .succeeded, "manual rips only warn")

        // Found through the history too (another output folder), but only while the folder is there.
        let elsewhere = root.appendingPathComponent("elsewhere", isDirectory: true)
        let empty = root.appendingPathComponent("empty-root", isDirectory: true)
        try FileManager.default.createDirectory(at: elsewhere, withIntermediateDirectories: true)
        try FileManager.default.createDirectory(at: empty, withIntermediateDirectories: true)
        let candidate = [ArchivedCandidate(fingerprint: fp, folder: elsewhere.path, state: .succeeded, finishedAt: Date())]
        let viaHistory = await archivedJob(fake, automatic: true, policy: .skip, candidates: candidate, root: empty)
        #expect(viaHistory.state == .cancelled && viaHistory.errorMessage?.contains(elsewhere.path) == true)
        try FileManager.default.removeItem(at: elsewhere)
        let empty2 = root.appendingPathComponent("empty-root-2", isDirectory: true)
        try FileManager.default.createDirectory(at: empty2, withIntermediateDirectories: true)
        #expect(await archivedJob(fake, automatic: true, policy: .skip, candidates: candidate, root: empty2).state == .succeeded)

        // Another disc of the set with the same label is not taken for the archived one.
        let other = try FakeMakeMKV(listing: listing(titles: [("0:31:00", 1)]), mkv: writesFile(saved))
        let otherJob = await archivedJob(other, automatic: true, policy: .skip, candidates: [ArchivedCandidate(fingerprint: fp, folder: root.path, state: .succeeded, finishedAt: Date())])
        #expect(otherJob.state == .succeeded && otherJob.fingerprint != fp)
    }
}
