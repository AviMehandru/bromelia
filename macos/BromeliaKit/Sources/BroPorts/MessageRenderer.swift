import BroDomain
import BroFoundation

/// The text of a message in the configured language (plan §16: shared/messages/en.json, rendered by the host's
/// adapter): for what the engine writes for people to read, such as the INCOMPLETE.txt note.
public protocol MessageRenderer: Sendable {
    func render(_ message: BroMessage) -> String
}
