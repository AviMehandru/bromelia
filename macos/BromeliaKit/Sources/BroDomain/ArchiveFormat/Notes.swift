import BroFoundation

/// The INCOMPLETE / READ ERRORS note, written into a folder whose files aren't a finished archive.
public enum Notes {
    /// The note's lines: the job and its outcome, a warning that the files are not a finished archive, the error, the
    /// read errors MakeMKV reported, and the log files copied into the folder (`logs`: the job's log, then MakeMKV's,
    /// when it was copied).
    public static func render(_ jobId: Id, outcome: Outcome, error: BroError?, readErrors: [RobotMessage], logs: [String]) -> [BroMessage] {
        var lines = [BroMessage(.archiveNoteTitle, [("jobId", .string(jobId.value)), ("outcome", .string(outcome.rawValue))]),
                     BroMessage(.archiveNoteNotFinished)]
        if let error { lines.append(BroMessage(MessageCode.parse(error.code) ?? .internalUnexpected, error.params, severity: .error)) }
        if !readErrors.isEmpty {
            lines.append(BroMessage(.archiveNoteReadErrors))
            lines += readErrors.map { BroMessage(.makemkvMessage, [("text", .string($0.text))], severity: .error) }
        }
        if let log = logs.first {
            lines.append(BroMessage(.archiveNoteLogs, [("log", .string(log)), ("hasMakemkv", .bool(logs.count > 1)),
                                                       ("makemkvLog", .string(logs.count > 1 ? logs[1] : ""))]))
        }
        return lines
    }
}
