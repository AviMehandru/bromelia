import BroDomain
import BroFoundation

/// Step 10 (plan §20.1; today's verify): checks what Acquire made before anything is named, hashed or moved. A kept
/// backup first (BackupCheck: backup.checkFailed). Then each title's MKV files, in plan order, against Probe's listing
/// title of the same index (MkvToolNix.probe, then RipCheck.check; ripcheck.checking): its notes as ripcheck.note
/// warnings, then ripcheck.checked and, with fewer tracks than the disc, ripcheck.trackSubset; a file mkvmerge can't read
/// (ripcheck.unreadable) or that doesn't match (ripcheck.mismatch) is rip.checkFailed, every title is still checked, and
/// the step fails with rip.failed or rip.titlesFailed. Without mkvmerge: ripcheck.cannotCheck and no MKV is checked, as
/// today. Skipped when Acquire left nothing. Data: {checked: [the names that passed]}.
public struct VerifyRipsStep: Step {
    public init() {}

    public var kind: StepKind { .verifyRips }

    public var resumability: Resumability { .idempotent }

    public func resources(_ job: JobSnapshot) -> ResourceSet { .none }

    public func run(_ context: StepContext) async throws(BroError) -> StepReport {
        guard let plan = PlanOutput.of(context.job), let acquired = AcquireOutput.of(context.job) else { return StepReport(skipped: true) }
        let staging = try context.library.stagingDirectory(plan.libraryId, jobId: context.job.id)
        let listing = ProbeOutput.decode(JobSnapshot.output(context.job, .probe) ?? .null).listing
        var passed: [String] = []
        if let backup = acquired.backup {
            if let problem = BackupCheck.problem(context.fs, path: staging + "/" + backup) {
                return failed(passed, BroMessage(.backupCheckFailed, [("problem", problem.toJson())], severity: .error))
            }
            passed.append(backup)
        }
        let files = acquired.titles.flatMap { t in t.files.map { (index: t.index, file: $0) } }
        var failures: [(title: Int, problem: BroMessage)] = []
        for (i, entry) in files.enumerated() {
            let file = entry.file
            context.progress.report(Double(i) / Double(files.count), operation: BroMessage(.ripcheckChecking, [("file", .string(file))]), current: nil)
            let probe: MkvProbe?
            do throws(BroError) {
                probe = try await context.tools.matroska.probe(staging + "/" + file, cancel: context.cancel)
            } catch {
                guard error.code == "tool.missing" || error.code == "tool.notFoundAt" else { throw error }
                context.log.log(BroMessage(.ripcheckCannotCheck, severity: .warning))
                return done(passed)
            }
            var failure: BroMessage?
            let title = listing?.titles.first { $0.index == entry.index }
            if probe == nil {
                failure = BroMessage(.ripcheckUnreadable, [("file", .string(file))])
            } else if let title, let probe {
                let result = RipCheck.check(probe, title: title)
                for note in result.notes { context.log.log(BroMessage(.ripcheckNote, [("file", .string(file)), ("note", note.toJson())], severity: .warning)) }
                if !result.problems.isEmpty {
                    failure = BroMessage(.ripcheckMismatch, [("file", .string(file)), ("problems", .array(result.problems.map { $0.toJson() }))])
                }
            }
            if let failure {
                context.log.log(BroMessage(.ripCheckFailed, [("title", .integer(Int64(entry.index))), ("problem", failure.toJson())], severity: .error))
                failures.append((entry.index, failure))
                continue
            }
            guard let probe else { continue }
            context.log.log(BroMessage(.ripcheckChecked, [("file", .string(file)), ("duration", .integer(Int64((probe.durationSeconds ?? 0).rounded()))),
                                                          ("tracks", .integer(Int64(probe.tracks.count))), ("chapters", .integer(Int64(probe.chapterCount)))]))
            if let title, !title.tracks.isEmpty, probe.tracks.count < title.tracks.count {
                context.log.log(BroMessage(.ripcheckTrackSubset, [("file", .string(file)), ("have", .integer(Int64(probe.tracks.count))),
                                                                  ("total", .integer(Int64(title.tracks.count)))]))
            }
            passed.append(file)
        }
        if failures.isEmpty { return done(passed) }
        func reason(_ f: (title: Int, problem: BroMessage)) -> JsonValue {
            BroMessage(.ripTitleReason, [("title", .string(String(f.title))), ("reason", f.problem.toJson())]).toJson()
        }
        return failed(passed, failures.count == 1
            ? BroMessage(.ripFailed, [("reason", reason(failures[0]))], severity: .error)
            : BroMessage(.ripTitlesFailed, [("count", .integer(Int64(failures.count))), ("reasons", .array(failures.map(reason)))], severity: .error))
    }

    private func data(_ passed: [String]) -> JsonValue { .object([("checked", .array(passed.map { .string($0) }))]) }

    private func done(_ passed: [String]) -> StepReport { StepReport(data: data(passed)) }

    private func failed(_ passed: [String], _ message: BroMessage) -> StepReport { StepReport(data: data(passed), error: message.toError()) }
}
