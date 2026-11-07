import BroDomain
import BroFoundation

/// Step 7 (plan §20.1), appended by Plan only when needed. It holds the plan's library for writing, so while that
/// library is offline the Scheduler keeps the job blocked (library.offline) and it goes on by itself when the library
/// is back. Then:
/// - When the library was offline at Plan: the archive check Plan couldn't make (ArchiveCheck: skip ends the job, ask
///   asks).
/// - An archived disc under automation.alreadyArchived ask: decision.archivedBefore {folder, day}, answers ripAgain
///   (logs disc.archivedBefore), skip and eject (both end the job with disc.alreadyArchivedAsk; Release at the end ejects
///   the disc for eject).
/// - The space Plan didn't reserve (SpaceCheck).
/// Data: {answer} (null when nothing was asked).
public struct DecideStep: Step {
    public static let archivedBefore = "archivedBefore"

    public init() {}

    public var kind: StepKind { .decide }

    public var resumability: Resumability { .idempotent }

    public func resources(_ job: JobSnapshot) -> ResourceSet {
        guard let plan = PlanOutput.of(job), !plan.libraryId.isEmpty else { return .none }
        return ResourceSet(libraryWrites: [plan.libraryId])
    }

    public func run(_ context: StepContext) async throws(BroError) -> StepReport {
        guard let plan = PlanOutput.of(context.job) else {
            throw BroError(MessageCode.wire(.internalUnexpected), [("detail", .string("decide without a plan"))])
        }
        let request = try StepSources.request(context.job)
        var configId: String?
        if case .drive(let id) = request.source { configId = context.driveLookup.find(id)?.configId }
        var ask = plan.askArchived
        if plan.libraryOffline, plan.mode != .infoOnly {
            let probe = ProbeOutput.decode(JobSnapshot.output(context.job, .probe) ?? .null)
            if let match = try await ArchiveCheck.find(context, probe.fingerprint) {
                if let skip = ArchiveCheck.apply(context, match, configId: configId) { return DecideStep.answered(nil, skip) }
                if ArchiveCheck.asks(context, configId: configId) { ask = match }
            }
        }
        var answer: String?
        if let ask {
            var question = ArchiveCheck.message(.decisionArchivedBefore, ask, withUnit: false)
            question.severity = .info
            let given = try await context.decisions.ask(DecisionDto(id: DecideStep.archivedBefore, question: question, answers: [
                DecisionAnswer(id: "ripAgain", label: BroMessage(.decisionAnswerRipAgain)),
                DecisionAnswer(id: "skip", label: BroMessage(.decisionAnswerSkip)),
                DecisionAnswer(id: "eject", label: BroMessage(.decisionAnswerEject)),
            ]))
            answer = given
            if given != "ripAgain" { return DecideStep.answered(given, ArchiveCheck.message(.discAlreadyArchivedAsk, ask, withUnit: true)) }
            context.log.log(ArchiveCheck.message(.discArchivedBefore, ask, withUnit: false))
        }
        if !plan.spaceReserved, let need = plan.spaceNeed {
            try await SpaceCheck.reserve(context, try context.library.require(plan.libraryId).path, need)
        }
        return DecideStep.answered(answer, nil)
    }

    static func answered(_ answer: String?, _ skip: BroMessage?) -> StepReport {
        StepReport(data: .object([("answer", answer.map { .string($0) } ?? .null)]), skipJob: skip)
    }
}
