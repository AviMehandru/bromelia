/// The severity of a MakeMKV message.
public enum MessageKind: String, Sendable, CaseIterable {
    case debug
    case info
    case warning
    case error
}
