/// A command step: the program, its interpreter (empty = run it directly), the arguments (split like a POSIX command
/// line before tokens are filled in), the working directory (empty = the unit's folder), whether it runs once per
/// file, and its environment.
public struct CommandSettings: Sendable, Equatable {
    public var executable = ""
    public var interpreter = ""
    public var arguments = "{outputDir}"
    public var workingDirectory = ""
    public var perFile = false
    public var environment: [String: String] = [:]

    public init() {}
}
