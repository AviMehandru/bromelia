/// The status given to user scripts (BROMELIA_STATUS), notifications and the manifest.
public enum StatusWord: String, Sendable, CaseIterable, Codable {
    case success
    case errors
    case failed
    case cancelled
    case skipped
    case interrupted
}
