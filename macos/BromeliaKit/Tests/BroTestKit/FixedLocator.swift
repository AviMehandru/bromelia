import BroDomain
import BroFoundation
import BroPorts

/// A locator that finds makemkvcon (and mkvmerge, when given) at a fixed path, with the version given, or nowhere, and no
/// other tool.
public struct FixedLocator: ToolLocator {
    public let makemkvcon: String?
    public let mkvmerge: String?
    public let makemkvconVersion: String?

    public init(makemkvcon: String?, mkvmerge: String? = nil, makemkvconVersion: String? = nil) {
        self.makemkvcon = makemkvcon
        self.mkvmerge = mkvmerge
        self.makemkvconVersion = makemkvconVersion
    }

    public func locate(_ tool: ToolKind) -> ToolInfo {
        if tool == .makemkvcon, let makemkvcon { return ToolInfo(tool: tool, path: makemkvcon, version: makemkvconVersion, capabilities: []) }
        if tool == .mkvmerge, let mkvmerge { return ToolInfo(tool: tool, path: mkvmerge, capabilities: []) }
        return ToolInfo(tool: tool, capabilities: [], why: BroMessage(.toolMissing, [("tool", .string(tool.rawValue))], severity: .warning))
    }
}
