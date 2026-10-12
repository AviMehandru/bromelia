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
