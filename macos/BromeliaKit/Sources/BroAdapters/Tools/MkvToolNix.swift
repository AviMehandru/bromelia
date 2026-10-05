import BroDomain
import BroFoundation
import BroPorts
import Foundation

/// MKVToolNix (plan §10.1; shared/fixtures/adapters/mkvtoolnix.cases.json): mkvmerge -J for the rip check, remuxing,
/// splitting at chapters, chapter times with mkvextract. Exit status 1 is mkvmerge's warning: the output is good.
/// tool.missing when the locator can't find a tool.
public final class MkvToolNix: Sendable {
    private let launcher: any ProcessLauncher
    private let fs: any FileSystem
    private let locator: any ToolLocator
    private let workDirectory: String

    /// - Parameter workDirectory: where chapterTimes writes mkvextract's chapter file (the job's folder).
    public init(launcher: any ProcessLauncher, fs: any FileSystem, locator: any ToolLocator, workDirectory: String) {
        self.launcher = launcher
        self.fs = fs
        self.locator = locator
        self.workDirectory = workDirectory
    }

    /// None when mkvmerge can't read the file.
    public func probe(_ file: String, cancel: CancellationToken) async throws(BroError) -> MkvProbe? {
        let (exit, lines) = try await run(.mkvmerge, ["-J", file], stall: 300, cancel)
        return exit.status <= 1 ? MkvProbe.parse(lines.joined(separator: "\n")) : nil
    }

    public func remux(_ arguments: [String], cancel: CancellationToken) async throws(BroError) -> Bool {
        try await run(.mkvmerge, arguments, stall: 600, cancel).0.status <= 1
    }

    /// The parts in order (hidden files next to the input); none when mkvmerge failed or made another number of parts,
    /// which are removed.
    public func split(_ chapters: [Int], input: String, cancel: CancellationToken) async throws(BroError) -> [String]? {
        let folder = (input as NSString).deletingLastPathComponent
        let prefix = ".bromelia-split-" + String(UUID().uuidString.lowercased().replacingOccurrences(of: "-", with: "").prefix(8))
        let (exit, _) = try await run(.mkvmerge, Split.arguments(chapters, input: input, output: folder + "/" + prefix + "-%03d.mkv"), stall: 600, cancel)
        let parts = ((try? fs.list(folder)) ?? []).map(\.name).filter { $0.hasPrefix(prefix) }
            .sorted { Array($0.utf8).lexicographicallyPrecedes(Array($1.utf8)) }.map { folder + "/" + $0 }
        if exit.status <= 1 && parts.count == chapters.count + 1 { return parts }
        for p in parts { try? fs.remove(p) }
        return nil
    }

    public func chapterTimes(_ file: String, cancel: CancellationToken) async throws(BroError) -> [Duration]? {
        let temp = workDirectory + "/.bromelia-chapters-" + String(UUID().uuidString.lowercased().prefix(8)) + ".txt"
        defer { if fs.exists(temp) { try? fs.remove(temp) } }
        let (exit, _) = try await run(.mkvextract, [file, "chapters", "--simple", temp], stall: 120, cancel)
        guard exit.status <= 1, fs.exists(temp), let bytes = try? fs.read(temp) else { return nil }
        return SimpleChapters.parse(String(decoding: bytes, as: UTF8.self))
    }

    private func run(_ tool: ToolKind, _ arguments: [String], stall: Double, _ cancel: CancellationToken) async throws(BroError) -> (ProcessExit, [String]) {
        let info = locator.locate(tool)
        guard let exe = info.path else {
            throw (info.why ?? BroMessage(.toolMissing, [("tool", .string(tool.rawValue))], severity: .warning)).toError()
        }
        if cancel.isCancelled { throw BroError(MessageCode.jobCancelled.rawValue) }
        var lines: [String] = []
        let exit = try await ToolRun.run(launcher, ProcessSpec(executable: exe, arguments: arguments, environment: [:], stopPolicy: .interruptFirst,
                                                               stallTimeout: Duration(seconds: stall)), cancel: cancel) { line in
            if line.stream == .stdout { lines.append(line.text) }
            return nil
        }
        if exit.cancelled { throw BroError(MessageCode.jobCancelled.rawValue) }
        return (exit, lines)
    }
}
