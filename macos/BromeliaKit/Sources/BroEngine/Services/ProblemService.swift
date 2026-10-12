import BroDomain
import BroFoundation
import BroPorts

/// The active problems every UI shows until they are cleared (plan §11.1), on the executor; a minimal one (owner,
/// 2026-10-12). In memory only: each service raises its problems again when it starts. A problem is keyed by its code
/// and scope; its id is the code, or `code:kind:id` with a scope. shared/fixtures/engine/problem-service.cases.json.
@EngineActor
public final class ProblemService {
    private let bus: EventBus
    private let clock: any Clock
    private var active: [ProblemDto] = []

    public init(bus: EventBus, clock: any Clock) {
        self.bus = bus
        self.clock = clock
    }

    /// The active problems, in the order they were first raised.
    public func problems() -> [ProblemDto] { active }

    /// Adds the problem and publishes problemRaised. The same one again does nothing; one with other params, severity
    /// or actions replaces it in place (same id, place and raisedAt) and publishes problemRaised again.
    @discardableResult
    public func raise(_ message: BroMessage, scope: ProblemScope? = nil, actions: [ProblemAction] = []) -> ProblemDto {
        let id = Self.key(message.code, scope)
        let i = active.firstIndex { $0.id == id }
        let problem = ProblemDto(id: id, message: message, scope: scope, raisedAt: i.map { active[$0].raisedAt } ?? clock.now(),
                                 actions: actions)
        if let i {
            if ProblemDto.json(active[i]) == ProblemDto.json(problem) { return active[i] }
            active[i] = problem
        } else {
            active.append(problem)
        }
        bus.publish(.problemRaised, data: .object([("problem", ProblemDto.json(problem))]))
        return problem
    }

    /// Removes the problem and publishes problemCleared; nothing when there is none.
    public func clear(_ code: MessageCode, scope: ProblemScope? = nil) {
        let id = Self.key(code, scope)
        guard active.contains(where: { $0.id == id }) else { return }
        active.removeAll { $0.id == id }
        bus.publish(.problemCleared, data: .object([("problemId", .string(id)), ("code", .string(code.rawValue))]))
    }

    private static func key(_ code: MessageCode, _ scope: ProblemScope?) -> String {
        guard let scope else { return code.rawValue }
        return code.rawValue + ":" + scope.kind.rawValue + ":" + scope.id
    }
}
