import BroDomain
import BroFoundation

/// A running job's progress (dtos.json Progress): `fraction` of the whole job, `currentFraction` of the current step.
public struct ProgressDto: Sendable, Equatable {
    public var step: StepKind
    public var stepIndex: Int
    public var stepCount: Int
    public var fraction: Double
    public var currentFraction: Double?
    public var operation: BroMessage
    public var current: String?
    public var secondsLeft: Double?

    public init(step: StepKind, stepIndex: Int, stepCount: Int, fraction: Double, currentFraction: Double?, operation: BroMessage,
                current: String? = nil, secondsLeft: Double? = nil) {
        self.step = step
        self.stepIndex = stepIndex
        self.stepCount = stepCount
        self.fraction = fraction
        self.currentFraction = currentFraction
        self.operation = operation
        self.current = current
        self.secondsLeft = secondsLeft
    }

    public static func json(_ progress: ProgressDto) -> JsonValue {
        var m: [(key: String, value: JsonValue)] = [("step", .string(progress.step.rawValue)), ("stepIndex", .integer(Int64(progress.stepIndex))),
                                                    ("stepCount", .integer(Int64(progress.stepCount))), ("fraction", JsonDto.number(progress.fraction))]
        if let c = progress.currentFraction { m.append(("currentFraction", JsonDto.number(c))) }
        m.append(("operation", progress.operation.toJson()))
        if let current = progress.current { m.append(("current", .string(current))) }
        if let left = progress.secondsLeft { m.append(("secondsLeft", JsonDto.number(left))) }
        return .object(m)
    }
}

/// Helpers the DTOs share.
enum JsonDto {
    /// A whole number as an integer, so 1.0 is written 1 on every platform.
    static func number(_ d: Double) -> JsonValue {
        d == d.rounded(.down) && abs(d) < 9e15 ? .integer(Int64(d)) : .number(d)
    }

    /// {code, params, cause} (shared/schema/common.json BroError), params left out when empty.
    static func error(_ error: BroError) -> JsonValue {
        var m: [(key: String, value: JsonValue)] = [("code", .string(error.code))]
        if !error.params.isEmpty { m.append(("params", .object(error.params))) }
        if let cause = error.cause { m.append(("cause", JsonDto.error(cause))) }
        return .object(m)
    }
}
