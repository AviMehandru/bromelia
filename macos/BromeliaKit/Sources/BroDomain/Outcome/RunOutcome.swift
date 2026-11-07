import BroFoundation

/// What one makemkvcon run amounted to. makemkvcon exits 0 when a title fails, so the saved / failed counts (5036,
/// 5037, 5004) and the files the run produced decide, not the exit status alone. `abandoned`: the process didn't end
/// after KILL and may still be writing, so what it wrote must never be reused or removed.
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
    public var abandoned: Bool

    public init(saved: Int? = nil, failed: Int? = nil, exitCode: Int, firstError: RobotMessage? = nil, spaceWarning: RobotMessage? = nil,
                readErrors: [RobotMessage] = [], driveMismatch: BroMessage? = nil, debugLog: String? = nil, status: StatusWord, error: BroMessage? = nil,
                produced: [String] = [], abandoned: Bool = false) {
        self.saved = saved
        self.failed = failed
        self.exitCode = exitCode
        self.firstError = firstError
        self.spaceWarning = spaceWarning
        self.readErrors = readErrors
        self.driveMismatch = driveMismatch
        self.debugLog = debugLog
        self.status = status
        self.error = error
        self.produced = produced
        self.abandoned = abandoned
    }

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
                       produced: produced, abandoned: exit.abandoned)
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
    /// ASCII case, that isn't hidden), the disc structure for a backup (a BDMV, VIDEO_TS or HVDVD_TS folder, or an .iso
    /// image), the file itself for an image (any name that isn't hidden), nothing for a listing. Anything else
    /// (.DS_Store, Thumbs.db, a partial file) doesn't count. Names are bytes, as in C# and C: hidden is a first byte '.'.
    public static func products(_ product: RunProduct, newNames: [String]) -> [String] {
        switch product {
        case .titles:
            return newNames.filter { !hidden($0) && ArchiveFiles.endsWith(asciiLower($0), ".mkv") }
        case .backup:
            return newNames.filter { n in
                !hidden(n) && (["bdmv", "video_ts", "hvdvd_ts"].contains { $0.utf8.elementsEqual(asciiLower(n).utf8) }
                               || ArchiveFiles.endsWith(asciiLower(n), ".iso"))
            }
        case .image:
            return newNames.filter { !hidden($0) }
        case .nothing:
            return []
        }
    }

    private static func hidden(_ name: String) -> Bool { name.utf8.first == UInt8(ascii: ".") }
}
