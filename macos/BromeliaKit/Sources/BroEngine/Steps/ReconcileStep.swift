import BroDomain
import BroFoundation

/// Step 3 (plan §12.4, §20.1), only when a session made the choices: the disc must still be the one that was opened
/// (disc.changed with why), and each ticked title must be found in the listing read now (ListingMatch.mapTitles;
/// hand-picked tracks must not have changed). Logs disc.titleNumbersChanged when MakeMKV numbers them differently
/// now. Skipped without a session, or when Probe had to use the opened listing. Data: ReconcileOutput.
public struct ReconcileStep: Step {
    public init() {}

    public var kind: StepKind { .reconcile }

    public var resumability: Resumability { .idempotent }

    public func resources(_ job: JobSnapshot) -> ResourceSet { .none }

    public func run(_ context: StepContext) async throws(BroError) -> StepReport {
        guard let session = try StepSources.request(context.job).session else { return StepReport(skipped: true) }
        let probe = ProbeOutput.decode(JobSnapshot.output(context.job, .probe) ?? .null)
        guard !probe.fromSession, let fresh = probe.listing else { return StepReport(skipped: true) }
        if let why = ListingMatch.differentDisc(session.listing, fresh) {
            return StepReport(error: BroError(MessageCode.wire(.discChanged), [("why", why.toJson())]))
        }
        let map: [Int: Int]
        do throws(BroError) {
            map = try ListingMatch.mapTitles(session.choices.titles.filter(\.selected).map(\.index), from: session.listing, to: fresh,
                                             sameTracks: Set(session.choices.titles.filter { $0.tracks != nil }.map(\.index)))
        } catch {
            return StepReport(error: error)
        }
        let moved = map.keys.sorted().filter { map[$0] != $0 }.map { JsonValue.string("\($0) → \(map[$0]!)") }
        if !moved.isEmpty { context.log.log(BroMessage(.discTitleNumbersChanged, [("changes", .array(moved))])) }
        return StepReport(data: ReconcileOutput.encode(ReconcileOutput(titleMap: map)))
    }
}
