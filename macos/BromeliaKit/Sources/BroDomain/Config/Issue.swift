import BroFoundation

/// A problem with the configuration: where (a JSON path such as `profiles[0].mode.default`), what (an issue code with
/// its parameters) and how bad (warning: ignored; error: the document is rejected).
public struct Issue: Sendable, Equatable {
    public var path: String
    public var code: MessageCode
    public var params: [(key: String, value: JsonValue)]
    public var severity: Severity

    public init(path: String, code: MessageCode, params: [(key: String, value: JsonValue)] = [], severity: Severity) {
        self.path = path
        self.code = code
        self.params = params
        self.severity = severity
    }

    public static func == (a: Issue, b: Issue) -> Bool {
        a.path == b.path && a.code == b.code && a.severity == b.severity && JsonValue.object(a.params) == JsonValue.object(b.params)
    }

    /// `{path, code, params?, severity}`, as the API's Issue.
    public func toJson() -> JsonValue {
        var members: [(key: String, value: JsonValue)] = [("path", .string(path)), ("code", .string(code.rawValue))]
        if !params.isEmpty { members.append(("params", .object(params))) }
        members.append(("severity", .string(severity.rawValue)))
        return .object(members)
    }
}
