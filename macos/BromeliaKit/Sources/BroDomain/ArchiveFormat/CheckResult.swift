/// The verdict of a unit's check (common.json's CheckResult).
public enum CheckResult: String, Sendable, CaseIterable {
    case ok
    case damaged
    case error
    case stopped
}
