import BroDomain
import BroFoundation

/// A line of a process's output, without its line break, and when it was read.
public struct OutputLine: Sendable, Equatable {
    public var stream: OutputSource
    public var text: String
    public var at: Instant

    public init(stream: OutputSource, text: String, at: Instant) {
        self.stream = stream
        self.text = text
        self.at = at
    }
}
