import BroDomain
import BroFoundation
import BroPorts

/// A locator that finds makemkvcon at a fixed path (or nowhere) and no other tool.
public struct FixedLocator: ToolLocator {
    public let makemkvcon: String?

    public init(makemkvcon: String?) { self.makemkvcon = makemkvcon }

    public func locate(_ tool: ToolKind) -> ToolInfo {
        if tool == .makemkvcon, let makemkvcon { return ToolInfo(tool: tool, path: makemkvcon, capabilities: []) }
        return ToolInfo(tool: tool, capabilities: [], why: BroMessage(.toolMissing, [("tool", .string(tool.rawValue))], severity: .warning))
    }
}
