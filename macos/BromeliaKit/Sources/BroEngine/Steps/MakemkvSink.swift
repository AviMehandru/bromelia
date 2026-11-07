import BroDomain
import BroFoundation
import BroPorts
import Foundation

/// Turns what makemkvcon says into the job's progress and log: every message but debug ones as makemkv.message with
/// its severity, the total progress as the step's fraction (under the given operation, with MakeMKV's current item),
/// and each new total operation as makemkv.titleProgress.
public final class MakemkvSink: RunSink, @unchecked Sendable {
    private let progress: any ProgressReporter
    private let log: any JobLogger
    private let operation: BroMessage
    private let lock = NSLock()
    private var current: String?
    private var total: String?

    public init(progress: any ProgressReporter, log: any JobLogger, operation: BroMessage) {
        self.progress = progress
        self.log = log
        self.operation = operation
    }

    /// MakeMKV starts its debug log (app_ShowDebug, named by message 1004) afresh at every start, so after each run the
    /// steps add it to the job folder's makemkv-debug.txt (SealStep.debugTranscript), after a line "==== <its path>
    /// after $ makemkvcon <command>" (the subcommand: info, mkv, backup). Nothing when the run named none or it is
    /// empty; a file that can't be read or written is left out (the run's result stands).
    public static func keepDebugLog(_ context: StepContext, _ outcome: RunOutcome, command: String) {
        guard let path = outcome.debugLog, !path.isEmpty else { return }
        let fs = context.fs
        do throws(BroError) {
            if !fs.exists(path) { return }
            let text = String(decoding: try fs.read(path), as: UTF8.self)
            if text.isEmpty { return }
            let file = context.jobDirectory + "/" + SealStep.debugTranscript
            let before = fs.exists(file) ? String(decoding: try fs.read(file), as: UTF8.self) : ""
            if !fs.exists(context.jobDirectory) { try fs.createDirectory(context.jobDirectory, parentsMustExist: false) }
            let added = "==== " + path + " after $ makemkvcon " + command + "\n" + text + (text.utf8.last == UInt8(ascii: "\n") ? "" : "\n")
            try fs.writeAtomically(file, bytes: Array((before + added).utf8), mode: 0o600) // as the job's other files
        } catch {
        }
    }

    public func event(_ event: RobotEvent) {
        lock.lock()
        defer { lock.unlock() }
        switch event {
        case .message(let m):
            let severity: Severity
            switch MessageCatalog.severity(m) {
            case .debug: return
            case .error: severity = .error
            case .warning: severity = .warning
            case .info: severity = .info
            }
            log.log(BroMessage(.makemkvMessage, [("text", .string(m.text))], severity: severity))
        case .progressTotal(_, _, let name):
            if name != total {
                total = name
                log.log(BroMessage(.makemkvTitleProgress, [("name", .string(name))]))
            }
        case .progressCurrent(_, _, let name):
            current = name
        case .progressValue(_, let t, let max) where max > 0:
            progress.report(min(1, Swift.max(0, Double(t) / Double(max))), operation: operation, current: current)
        default:
            break
        }
    }
}
