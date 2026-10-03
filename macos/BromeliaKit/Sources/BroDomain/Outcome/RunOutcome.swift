import BroFoundation

/// What one makemkvcon run amounted to. makemkvcon exits 0 when a title fails, so the saved / failed counts (5036,
/// 5037, 5004) and the files the run produced decide, not the exit status alone.
public struct RunOutcome: Sendable, Equatable {
    public var saved: Int?
    public var failed: Int?
    public var exitCode: Int
    public var firstError: RobotMessage?
    public var spaceWarning: RobotMessage?
    public var readErrors: [RobotMessage]
    public var driveMismatch: BroMessage?
    public var debugLog: String?
    public var status: StatusWord
    public var error: BroMessage?

    /// Cancelled when the run was cancelled. Failed when MakeMKV's space warning or a renumbered drive stopped it,
    /// when it stalled, exited non-zero, reported a failed title or produced nothing; the error is the reason (the
    /// first error MakeMKV reported, else the last, else the exit status). Errors when it read the disc with read
    /// errors (rip.readErrors). Success otherwise.
    public static func classify(_ a: RunAccumulator, exit: ProcessExit, producedFiles: Int) -> RunOutcome {
        func with(_ status: StatusWord, _ error: BroMessage?) -> RunOutcome {
            RunOutcome(saved: a.saved, failed: a.failed, exitCode: exit.status, firstError: a.firstError, spaceWarning: a.spaceWarning,
                       readErrors: a.readErrors, driveMismatch: a.driveMismatch, debugLog: a.debugLog, status: status, error: error)
        }
        // These stop makemkvcon themselves, so they come before cancellation.
        if let mismatch = a.driveMismatch { return with(.failed, mismatch) }
        if let space = a.spaceWarning { return with(.failed, BroMessage(.spaceMakemkvWarning, [("text", .string(space.text))], severity: .error)) }
        if let silence = exit.stalled {
            return with(.failed, BroMessage(.makemkvStalled, [("minutes", .integer(Int64(silence.seconds / 60))), ("abandoned", .bool(exit.abandoned))],
                                            severity: .error))
        }
        if exit.cancelled || exit.abandoned { return with(.cancelled, nil) }
        if exit.status != 0 || (a.failed ?? 0) > 0 || producedFiles == 0 {
            if let reason = a.firstError ?? a.errors.last {
                return with(.failed, BroMessage(.makemkvMessage, [("text", .string(reason.text))], severity: .error))
            }
            return with(.failed, BroMessage(.processExitStatus, [("status", .integer(Int64(exit.status)))], severity: .error))
        }
        if !a.readErrors.isEmpty {
            return with(.errors, BroMessage(.ripReadErrors, [("count", .integer(Int64(a.readErrors.count)))], severity: .error))
        }
        return with(.success, nil)
    }
}
