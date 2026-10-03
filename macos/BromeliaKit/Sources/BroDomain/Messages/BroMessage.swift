import BroFoundation

/// What logs, progress, problems and notifications carry (plan §16): a code, its parameters and a severity.
/// The engine never builds sentences; clients render the code from shared/messages.
public struct BroMessage: Sendable, Equatable {
    public var code: MessageCode
    public var params: [(key: String, value: JsonValue)]
    public var severity: Severity

    public init(_ code: MessageCode, _ params: [(key: String, value: JsonValue)] = [], severity: Severity = .info) {
        self.code = code
        self.params = params
        self.severity = severity
    }

    public static func == (a: BroMessage, b: BroMessage) -> Bool {
        a.code == b.code && a.severity == b.severity && JsonValue.object(a.params) == JsonValue.object(b.params)
    }

    /// The message as an expected failure.
    public func toError(cause: BroError? = nil) -> BroError { BroError(code.rawValue, params, cause: cause) }

    /// `{"code": …, "params": {…}}`, as in shared/schema/common.json (params left out when empty).
    public func toJson() -> JsonValue {
        var members: [(key: String, value: JsonValue)] = [("code", .string(code.rawValue))]
        if !params.isEmpty { members.append(("params", .object(params))) }
        return .object(members)
    }
}
