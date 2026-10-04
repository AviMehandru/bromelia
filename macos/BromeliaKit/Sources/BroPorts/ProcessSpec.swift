import BroDomain
import BroFoundation

/// A process to start, never through a shell: the program and its arguments, environment variables added to the
/// engine's, the working directory, how it is stopped, how long it may stay silent before it is stopped as stalled, the
/// transcript file every line is appended to (headed by the command line), and the interpreter for a script.
public struct ProcessSpec: Sendable, Equatable {
    public var executable: String
    public var arguments: [String]
    public var environment: [String: String]
    public var workingDirectory: String?
    public var stopPolicy: StopPolicy
    public var stallTimeout: Duration?
    public var transcript: String?
    public var interpreter: String?

    public init(executable: String, arguments: [String], environment: [String: String],
                workingDirectory: String? = nil, stopPolicy: StopPolicy, stallTimeout: Duration? = nil,
                transcript: String? = nil, interpreter: String? = nil) {
        self.executable = executable
        self.arguments = arguments
        self.environment = environment
        self.workingDirectory = workingDirectory
        self.stopPolicy = stopPolicy
        self.stallTimeout = stallTimeout
        self.transcript = transcript
        self.interpreter = interpreter
    }
}
