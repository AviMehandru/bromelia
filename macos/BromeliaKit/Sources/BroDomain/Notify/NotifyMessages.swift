import BroFoundation

/// The text of notifications, as message codes.
public enum NotifyMessages {
    /// "MKV finished: Inception", then what happened to the files: saved (also when a post-processing step failed),
    /// kept with read errors, or the job's error (else the outcome).
    public static func forJob(_ job: JobSummary, outcome: Outcome) -> TitleAndBody {
        let title = BroMessage(.notifyTitleJob, [("mode", .string(job.mode.rawValue)), ("outcome", .string(outcome.rawValue)), ("what", .string(job.what))])
        let line: BroMessage
        switch outcome {
        case .succeeded, .succeededStepsFailed:
            line = BroMessage(.notifyBodySaved, [("count", .integer(Int64(job.files))), ("path", .string(job.path))])
        case .succeededWithReadErrors:
            line = BroMessage(.notifyBodyReadErrors, [("path", .string(job.path))])
        case .failed, .cancelled, .skipped, .interrupted:
            line = BroMessage(.notifyBodyError, [("error", (job.error ?? outcomeMessage(outcome)).toJson())])
        }
        return TitleAndBody(title: title, lines: [line])
    }

    static func outcomeMessage(_ outcome: Outcome) -> BroMessage {
        switch outcome {
        case .cancelled: return BroMessage(.outcomeCancelled)
        case .skipped: return BroMessage(.outcomeSkipped)
        case .interrupted: return BroMessage(.outcomeInterrupted)
        default: return BroMessage(.outcomeFailed)
        }
    }

    /// "Archive check: all 12 folder(s) OK", or how many are damaged with a line for each of the first ten (a folder
    /// that isn't OK counts as damaged).
    public static func forCheck(_ results: [FolderCheck]) -> TitleAndBody {
        let bad = results.filter { $0.result.result != .ok }
        if bad.isEmpty {
            return TitleAndBody(title: BroMessage(.checkNotifyAllOk, [("total", .integer(Int64(results.count)))]), lines: [BroMessage(.checkNotifyEveryFileMatches)])
        }
        var lines = bad.prefix(10).map { BroMessage(.checkNotifyFolder, [("folder", .string($0.folder)), ("summary", $0.result.summary.toJson())]) }
        if bad.count > 10 { lines.append(BroMessage(.checkNotifyMore, [("count", .integer(Int64(bad.count - 10)))])) }
        return TitleAndBody(title: BroMessage(.checkNotifyDamaged, [("damaged", .integer(Int64(bad.count))), ("total", .integer(Int64(results.count)))]),
                            lines: lines)
    }
}
