import BroAdapters
import BroDomain
import BroFoundation
import BroPorts
import Foundation
import Testing

/// The adapters together with the real makemkvcon on a disc image: only when BROMELIA_TEST_ISO names one (CLAUDE.md,
/// "Real-disc tests"). No drive is touched: the source is iso:<path>. The settings are empty, so the owner's MakeMKV
/// settings and key are never read.
struct RealIsoTests {
    static let iso = ProcessInfo.processInfo.environment["BROMELIA_TEST_ISO"] ?? ""

    final class Events: RunSink, @unchecked Sendable {
        private let lock = NSLock()
        private var n = 0
        var count: Int { lock.withLock { n } }
        func event(_ event: RobotEvent) { lock.withLock { n += 1 } }
    }

    @Test(.enabled(if: !iso.isEmpty)) func listsTheImageWithTheRealMakemkvcon() async throws {
        let work = URL(fileURLWithPath: NSTemporaryDirectory()).resolvingSymlinksInPath().path + "/bromelia-iso-" + UUID().uuidString
        defer { try? FileManager.default.removeItem(atPath: work) }
        let fs = PlatformFileSystem()
        let locator = SystemToolLocator(fs: fs, configured: [:], candidates: PlatformToolPaths.candidates(NSHomeDirectory()),
                                        names: PlatformToolPaths.names(),
                                        searchPath: (ProcessInfo.processInfo.environment["PATH"] ?? "").split(separator: ":").map(String.init),
                                        home: NSHomeDirectory())
        let makemkvcon = try #require(locator.locate(.makemkvcon).path)
        let tool = MakemkvTool(launcher: PlatformProcessLauncher(), fs: fs, isolation: HomeDirIsolation(fs: fs, layout: .macos), locator: locator)
        let invocation = MakemkvInvocation(settings: MakemkvRunSettings(settings: [:], dataDir: "", workDirectory: work + "/home"),
                                           options: MakemkvOptions(), stallTimeout: Duration(seconds: 300), transcript: work + "/makemkv.txt")
        let events = Events()
        let result = try await tool.listing(.iso(path: Self.iso), invocation: invocation, sink: events, cancel: CancellationToken())
        #expect(result.run.outcome.status == .success)
        #expect(result.listing.titles.count > 0)
        #expect(events.count > 0)
        let fingerprint = Fingerprint.of(result.listing) ?? ""
        print("makemkvcon \(makemkvcon): \(result.listing.titles.count) titles, \(result.run.version ?? "?"), fingerprint \(fingerprint)")
        #expect(fingerprint.hasPrefix("v1:c13d733d") || fingerprint.hasPrefix("v1:742cbae9"))
        let transcript = try String(contentsOfFile: work + "/makemkv.txt", encoding: .utf8)
        #expect(transcript.hasPrefix("==== "))
        #expect(transcript.contains(" exit status 0\n"))
        #expect(FileManager.default.fileExists(atPath: work + "/home/Library/MakeMKV/settings.conf"))
    }

    /// The smallest title, ripped: MakeMKV's saved count and the new MKV file agree, so the run succeeds and the file is
    /// what it produced.
    @Test(.enabled(if: !iso.isEmpty)) func ripsTheSmallestTitleWithTheRealMakemkvcon() async throws {
        let work = URL(fileURLWithPath: NSTemporaryDirectory()).resolvingSymlinksInPath().path + "/bromelia-iso-" + UUID().uuidString
        defer { try? FileManager.default.removeItem(atPath: work) }
        let fs = PlatformFileSystem()
        let locator = SystemToolLocator(fs: fs, configured: [:], candidates: PlatformToolPaths.candidates(NSHomeDirectory()),
                                        names: PlatformToolPaths.names(),
                                        searchPath: (ProcessInfo.processInfo.environment["PATH"] ?? "").split(separator: ":").map(String.init),
                                        home: NSHomeDirectory())
        let tool = MakemkvTool(launcher: PlatformProcessLauncher(), fs: fs, isolation: HomeDirIsolation(fs: fs, layout: .macos), locator: locator)
        let invocation = MakemkvInvocation(settings: MakemkvRunSettings(settings: [:], dataDir: "", workDirectory: work + "/home"),
                                           options: MakemkvOptions(), stallTimeout: Duration(seconds: 300), transcript: work + "/makemkv.txt")
        let listing = try await tool.listing(.iso(path: Self.iso), invocation: invocation, sink: Events(), cancel: CancellationToken()).listing
        let smallest = try #require(listing.titles.min { $0.sizeBytes < $1.sizeBytes })
        try FileManager.default.createDirectory(atPath: work + "/staging", withIntermediateDirectories: true)
        FileManager.default.createFile(atPath: work + "/staging/.DS_Store", contents: Data())
        let run = try await tool.rip(.iso(path: Self.iso), title: String(smallest.index), destination: work + "/staging", invocation: invocation,
                                     sink: Events(), cancel: CancellationToken())
        print("title \(smallest.index) (\(smallest.sizeBytes) bytes): \(run.outcome.status), saved \(run.outcome.saved.map(String.init) ?? "none"), produced \(run.outcome.produced)")
        #expect(run.outcome.status == .success)
        #expect(run.outcome.saved == 1)
        #expect(run.outcome.produced.count == 1 && run.outcome.produced[0].hasSuffix(".mkv"))
    }
}
