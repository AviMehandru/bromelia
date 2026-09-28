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
        #expect(allItems(root.appendingPathComponent(sub)).contains("title_t00.mkv"), "\(allItems(root.appendingPathComponent(sub)))")
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
}
