import BroDomain
import BroFoundation

/// A line of a job's log (dtos.json LogLine). jobs/<id>/log.jsonl holds the same objects without their seq.
public struct LogLineDto: Sendable, Equatable {
    public var seq: Int64
    public var t: Instant
    public var severity: Severity
    public var code: MessageCode
    public var params: [(key: String, value: JsonValue)]

    public init(seq: Int64, t: Instant, severity: Severity, code: MessageCode, params: [(key: String, value: JsonValue)]) {
        self.seq = seq
        self.t = t
        self.severity = severity
        self.code = code
        self.params = params
    }

    public static func == (a: LogLineDto, b: LogLineDto) -> Bool {
        a.seq == b.seq && a.t == b.t && a.severity == b.severity && a.code == b.code && JsonValue.object(a.params) == JsonValue.object(b.params)
    }

    public static func json(_ line: LogLineDto) -> JsonValue {
        var m: [(key: String, value: JsonValue)] = [("seq", .integer(line.seq)), ("t", .string(Instant.format(line.t))),
                                                    ("severity", .string(line.severity.rawValue)), ("code", .string(line.code.rawValue))]
        if !line.params.isEmpty { m.append(("params", .object(line.params))) }
        return .object(m)
    }
}
