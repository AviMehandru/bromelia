/// When a step runs: success (succeeded); failure (failed, cancelled, interrupted, or succeeded with read errors);
/// always. Skipped jobs never run steps.
public enum RunOn: String, Sendable, CaseIterable {
    case success
    case failure
    case always
}
