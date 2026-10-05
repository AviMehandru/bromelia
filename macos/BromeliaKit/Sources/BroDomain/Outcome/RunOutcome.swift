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
    public var produced: [String]

    /// Cancelled when the run was cancelled. Failed when MakeMKV's space warning or a renumbered drive stopped it,
    /// when it stalled, exited non-zero, reported a failed title or produced nothing; the error is the reason (the
    /// first error MakeMKV reported, else the last, else the exit status, else process.savedNothing). A rip (`.titles`)
    /// also fails when MakeMKV didn't say how many titles it saved (makemkv.noSummary) or when that number isn't the
    /// number of new MKV files (makemkv.savedMismatch). Errors when it read the disc with read errors
    /// (rip.readErrors). Success otherwise. `newNames` are the names in the destination that weren't there before the
    /// run; only `products` of them count, and they become `produced`.
    public static func classify(_ a: RunAccumulator, exit: ProcessExit, product: RunProduct, newNames: [String]) -> RunOutcome {
        let produced = products(product, newNames: newNames)
        func with(_ status: StatusWord, _ error: BroMessage?) -> RunOutcome {
            RunOutcome(saved: a.saved, failed: a.failed, exitCode: exit.status, firstError: a.firstError, spaceWarning: a.spaceWarning,
                       readErrors: a.readErrors, driveMismatch: a.driveMismatch, debugLog: a.debugLog, status: status, error: error,
                       produced: produced)
        }
        // These stop makemkvcon themselves, so they come before cancellation.
        if let mismatch = a.driveMismatch { return with(.failed, mismatch) }
        if let space = a.spaceWarning { return with(.failed, BroMessage(.spaceMakemkvWarning, [("text", .string(space.text))], severity: .error)) }
        if let silence = exit.stalled {
            return with(.failed, BroMessage(.makemkvStalled, [("minutes", .integer(Int64(silence.seconds / 60))), ("abandoned", .bool(exit.abandoned))],
                                            severity: .error))
        }
        if exit.cancelled || exit.abandoned { return with(.cancelled, nil) }
        if exit.status != 0 || (a.failed ?? 0) > 0 || (product != .nothing && produced.isEmpty) {
            if let reason = a.firstError ?? a.errors.last {
                return with(.failed, BroMessage(.makemkvMessage, [("text", .string(reason.text))], severity: .error))
            }
            if exit.status != 0 { return with(.failed, BroMessage(.processExitStatus, [("status", .integer(Int64(exit.status)))], severity: .error)) }
            return with(.failed, BroMessage(.processSavedNothing, [("tool", .string("makemkvcon"))], severity: .error))
        }
        if product == .titles {
            guard let saved = a.saved else { return with(.failed, BroMessage(.makemkvNoSummary, severity: .error)) }
            if saved != produced.count {
                return with(.failed, BroMessage(.makemkvSavedMismatch, [("saved", .integer(Int64(saved))), ("produced", .integer(Int64(produced.count)))],
                                                severity: .error))
            }
        }
        if !a.readErrors.isEmpty {
            return with(.errors, BroMessage(.ripReadErrors, [("count", .integer(Int64(a.readErrors.count)))], severity: .error))
        }
        return with(.success, nil)
    }

    /// The new names a run of this kind produced, in the order given: MKV files for a rip (a name ending in .mkv, in any
    /// case, that isn't hidden), the disc structure for a backup (a BDMV, VIDEO_TS or HVDVD_TS folder, or an .iso
    /// image), nothing for a listing. Anything else (.DS_Store, Thumbs.db, a partial file) doesn't count.
    public static func products(_ product: RunProduct, newNames: [String]) -> [String] {
        switch product {
        case .titles:
            return newNames.filter { !$0.hasPrefix(".") && $0.lowercased().hasSuffix(".mkv") }
        case .backup:
            return newNames.filter { !$0.hasPrefix(".") && (["BDMV", "VIDEO_TS", "HVDVD_TS"].contains($0.uppercased()) || $0.lowercased().hasSuffix(".iso")) }
        case .nothing:
            return []
        }
    }
}
