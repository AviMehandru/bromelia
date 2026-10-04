import BroDomain
import BroFoundation
import BroPorts
import Foundation

/// Audio CDs (plan §10.1; shared/fixtures/adapters/cd-ripper.cases.json): the profile's audio CD command, else cyanrip,
/// else abcde, run in the destination; both look the album up in MusicBrainz and name the files themselves.
public final class CdRipper: Sendable {
    private let launcher: any ProcessLauncher
    private let locator: any ToolLocator
    private let fs: any FileSystem

    public init(launcher: any ProcessLauncher, locator: any ToolLocator, fs: any FileSystem) {
        self.launcher = launcher
        self.locator = locator
        self.fs = fs
    }

    /// Rips the CD in `device` into `dest` (which must exist) and returns the visible items saved there, sorted.
    /// `command`: the profile's audio CD command ({device} is the drive), empty for none; `stallMinutes`: 0 for no
    /// stall timeout.
    public func rip(_ device: String, dest: String, command: String?, stallMinutes: Int, sink: any RunSink, cancel: CancellationToken) async throws(BroError)
        -> [String]
    {
        var located: [String: String] = [:]
        for tool in [ToolKind.cyanrip, .abcde] {
            if let path = locator.locate(tool).path { located[tool.rawValue] = path }
        }
        let available = [ToolKind.cyanrip, .abcde].map(\.rawValue).filter { located[$0] != nil }
        guard let line = CdRipperArgs.build(.object([("audioCommand", .string(command ?? ""))]), device: device, available: available) else {
            throw BroMessage(.otherAudioNeedsRipper, [("platform", .string("macos"))], severity: .error).toError()
        }
        let name = line.executable
        let exe = located[name] ?? ToolKind(rawValue: name).flatMap { [.cyanrip, .abcde].contains($0) ? locator.locate($0).path : nil } ?? name
        if cancel.isCancelled { throw BroError(MessageCode.jobCancelled.rawValue) }
        let process = try launcher.start(ProcessSpec(executable: exe, arguments: line.arguments, environment: [:], workingDirectory: dest,
                                                     stopPolicy: .interruptFirst,
                                                     stallTimeout: stallMinutes > 0 ? Duration(seconds: Double(stallMinutes * 60)) : nil))
        let remove = cancel.onCancel { process.stop(.cancelled) }
        defer { remove() }
        for await output in process.lines() { sink.event(.raw(text: output.text)) }
        let exit = await process.wait()
        if exit.cancelled { throw BroError(MessageCode.jobCancelled.rawValue) }
        if exit.stalled != nil { throw failed(.processStalled, name, [("minutes", .integer(Int64(stallMinutes)))]) }
        if exit.status != 0 { throw failed(.processFailed, name, [("status", .integer(Int64(exit.status)))]) }
        let saved = ((try? fs.list(dest)) ?? []).map(\.name).filter { !$0.hasPrefix(".") }
            .sorted { Array($0.utf8).lexicographicallyPrecedes(Array($1.utf8)) }.map { (dest as NSString).appendingPathComponent($0) }
        if saved.isEmpty { throw failed(.processSavedNothing, name, []) }
        return saved
    }

    private func failed(_ code: MessageCode, _ tool: String, _ more: [(String, JsonValue)]) -> BroError {
        BroMessage(code, [("tool", .string(tool))] + more, severity: .error).toError()
    }
}
