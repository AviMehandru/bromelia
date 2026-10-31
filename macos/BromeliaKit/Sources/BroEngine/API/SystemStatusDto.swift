import BroDomain
import BroFoundation
import BroPorts

/// The engine as clients see it (dtos.json SystemStatus): the facts the host gave, when start ran, whether MakeMKV is
/// found and its version, the tools phase 3 runs as the ToolLocator finds them (makemkvcon, mkvmerge), the problems, the
/// libraries, the configuration's version and the last event's seq. MakeMKV's registration is unknown until
/// MakemkvService's phase 5 part.
public struct SystemStatusDto: Sendable, Equatable {
    public var facts: EngineFacts
    public var startedAt: Instant
    public var makemkvFound: Bool
    public var makemkvVersion: String?
    public var tools: [ToolInfo]
    public var problems: [ProblemDto]
    public var libraries: [LibraryDto]
    public var configVersion: Int64
    public var eventSeq: Int64

    public init(facts: EngineFacts, startedAt: Instant, makemkvFound: Bool, makemkvVersion: String?, tools: [ToolInfo],
                problems: [ProblemDto], libraries: [LibraryDto], configVersion: Int64, eventSeq: Int64) {
        self.facts = facts
        self.startedAt = startedAt
        self.makemkvFound = makemkvFound
        self.makemkvVersion = makemkvVersion
        self.tools = tools
        self.problems = problems
        self.libraries = libraries
        self.configVersion = configVersion
        self.eventSeq = eventSeq
    }

    /// Optional members left out when absent.
    public static func json(_ status: SystemStatusDto) -> JsonValue {
        var makemkv: [(key: String, value: JsonValue)] = [("found", .bool(status.makemkvFound))]
        if let version = status.makemkvVersion { makemkv.append(("version", .string(version))) }
        makemkv.append(("registration", .string("unknown")))
        return .object([
            ("version", .string(status.facts.version)),
            ("engine", .object([("pid", .integer(status.facts.pid)), ("startedAt", .string(Instant.format(status.startedAt))),
                                ("platform", .string(status.facts.platform)), ("dataDir", .string(status.facts.dataDir))])),
            ("makemkv", .object(makemkv)),
            ("tools", .array(status.tools.map(toolJson))),
            ("problems", .array(status.problems.map(ProblemDto.json))),
            ("libraries", .array(status.libraries.map(LibraryDto.json))),
            ("configVersion", .integer(status.configVersion)),
            ("eventSeq", .integer(status.eventSeq)),
        ])
    }

    /// dtos.json ToolInfo: found when it has a path; why (with its severity) when the locator gave one.
    private static func toolJson(_ tool: ToolInfo) -> JsonValue {
        var m: [(key: String, value: JsonValue)] = [("tool", .string(tool.tool.rawValue)), ("found", .bool(tool.path != nil))]
        if let path = tool.path { m.append(("path", .string(path))) }
        if let version = tool.version { m.append(("version", .string(version))) }
        if let why = tool.why { m.append(("why", .object((why.toJson().members ?? []) + [("severity", .string(why.severity.rawValue))]))) }
        return .object(m)
    }
}
