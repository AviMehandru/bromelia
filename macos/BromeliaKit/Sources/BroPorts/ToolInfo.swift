import BroDomain
import BroFoundation

/// Where a tool is (none: not found), its version, what it can do, and why it was or wasn't found.
public struct ToolInfo: Sendable, Equatable {
    public var tool: ToolKind
    public var path: String?
    public var version: String?
    public var capabilities: [String]
    public var why: BroMessage?

    public init(tool: ToolKind, path: String? = nil, version: String? = nil, capabilities: [String],
                why: BroMessage? = nil) {
        self.tool = tool
        self.path = path
        self.version = version
        self.capabilities = capabilities
        self.why = why
    }
}
