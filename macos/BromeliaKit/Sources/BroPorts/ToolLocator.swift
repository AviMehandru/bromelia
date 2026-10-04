import BroDomain
import BroFoundation

/// Finds the external tools.
public protocol ToolLocator: Sendable {
    func locate(_ tool: ToolKind) -> ToolInfo
}
