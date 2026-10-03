/// A program and its arguments. A bare program name is resolved by the adapter (ToolLocator).
public struct CommandLine: Sendable, Equatable {
    public var executable: String
    public var arguments: [String]

    public init(executable: String, arguments: [String]) {
        self.executable = executable
        self.arguments = arguments
    }
}
