/// What hashing one listed file found.
public enum FileVerdict: String, Sendable, CaseIterable {
    case same
    case changed
    case missing
    case unreadable
}
