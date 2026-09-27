import Testing
import Foundation
@testable import Bromelia

/// End-to-end test against a real disc image. Runs only when BROMELIA_TEST_ISO points to an ISO
/// (pass it to xcodebuild as TEST_RUNNER_BROMELIA_TEST_ISO=/path/to/disc.iso) and makemkvcon is installed.
/// Optional: BROMELIA_TEST_TITLE (default 1) and BROMELIA_TEST_TRACKS (comma separated stream indices).
@Suite("Integration", .serialized)
@MainActor
struct IntegrationTests {
    init() {
        Paths.dataOverride = FileManager.default.temporaryDirectory.appendingPathComponent("bromelia-test-data", isDirectory: true)
    }

    nonisolated static var iso: String? { ProcessInfo.processInfo.environment["BROMELIA_TEST_ISO"] }

    @Test(.enabled(if: IntegrationTests.iso != nil, "set BROMELIA_TEST_ISO to run"))
    func ripsTitleWithCustomTracksRenameAndScript() async throws {
        let env = ProcessInfo.processInfo.environment
        let iso = try #require(Self.iso)
        let exe = try #require(Paths.resolveTool(configured: "", candidates: Paths.makemkvconCandidates), "makemkvcon not installed")
        let mkvmerge = Paths.resolveTool(configured: "", candidates: Paths.mkvmergeCandidates)
        let titleIndex = Int(env["BROMELIA_TEST_TITLE"] ?? "1") ?? 1
        let keep = Set((env["BROMELIA_TEST_TRACKS"] ?? "0,1").split(separator: ",").compactMap { Int($0) })

        let out = FileManager.default.temporaryDirectory.appendingPathComponent("bromelia-it-\(UUID().uuidString.prefix(6))")
        defer { try? FileManager.default.removeItem(at: out) }
        let marker = out.appendingPathComponent("post.txt")

        var config = AppConfig()
        config.outputRoot = out.path
        var drive = DriveConfig()
        drive.name = "Test drive"
        drive.output.folderTemplate = "{type}/{disc}"
        drive.output.fileNameTemplate = "{disc} - {n:2}"
        drive.automation.notify = false
        drive.settings["dvd_MinimumTitleLength"] = "120"
        var step = PostProcessStep()
        step.name = "record"
        step.executable = "/bin/sh"
        step.arguments = "-c 'echo \"$BROMELIA_STATUS|$BROMELIA_FILE_COUNT|$1\" > \"$0\"' \(marker.path) {outputDir}"
        drive.postProcess = [step]

        let job = RipJob(source: .iso(path: iso), drive: drive, laneKey: "iso:test", sourceLabel: "test", discLabel: "", mode: .mkv)
        job.manualTitles = [titleIndex]
        if mkvmerge != nil { job.trackSelections = [titleIndex: keep] }
        let runner = JobRunner(job: job, config: config, makemkvcon: exe, mkvmerge: mkvmerge)
        await runner.run()

        #expect(job.state == .succeeded, "\(job.errorMessage ?? "") \n\(job.log.suffix(20).map(\.text).joined(separator: "\n"))")
        #expect(job.producedFiles.count == 1)
        let file = try #require(job.producedFiles.first)
        #expect(file.lastPathComponent == "\(job.discLabel) - 01.mkv")
        #expect(file.deletingLastPathComponent().deletingLastPathComponent().lastPathComponent == "dvd" || job.discInfo?.typeToken != "dvd")
        #expect(FileManager.default.fileExists(atPath: file.path))

        if let mkvmerge {
            let layout = await Remuxer.identify(mkvmerge: mkvmerge, file: file)
            #expect(layout?.count == keep.count)
        }
        let post = try String(contentsOf: marker, encoding: .utf8)
        #expect(post.hasPrefix("success|1|\(job.outputDirectory!.path)"))
        let manifest = try Data(contentsOf: job.manifestFile)
        let m = try ConfigStore.decoder().decode(JobManifest.self, from: manifest)
        #expect(m.status == "success")
        #expect(m.files == [file.path])
    }

    @Test(.enabled(if: IntegrationTests.iso != nil, "set BROMELIA_TEST_ISO to run"))
    func failsCleanlyWhenNoTitleMatches() async throws {
        let iso = try #require(Self.iso)
        let exe = try #require(Paths.resolveTool(configured: "", candidates: Paths.makemkvconCandidates))
        let out = FileManager.default.temporaryDirectory.appendingPathComponent("bromelia-it-\(UUID().uuidString.prefix(6))")
        defer { try? FileManager.default.removeItem(at: out) }
        var config = AppConfig()
        config.outputRoot = out.path
        var drive = DriveConfig()
        drive.automation.notify = false
        drive.rip.titleSelection.minDurationSeconds = 99 * 3600
        var step = PostProcessStep()
        step.executable = "/usr/bin/touch"
        step.arguments = out.appendingPathComponent("failed-marker").path
        step.runOn = .failure
        drive.postProcess = [step]
        let job = RipJob(source: .iso(path: iso), drive: drive, laneKey: "iso:test2", sourceLabel: "test", discLabel: "", mode: .mkv)
        let runner = JobRunner(job: job, config: config, makemkvcon: exe, mkvmerge: nil)
        await runner.run()
        #expect(job.state == .failed)
        #expect(job.errorMessage == "No titles matched the title selection rules")
        #expect(FileManager.default.fileExists(atPath: out.appendingPathComponent("failed-marker").path))
    }
}
