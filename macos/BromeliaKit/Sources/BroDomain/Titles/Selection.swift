/// The titles the rules chose (in index order), the reason for every title, whether the user must choose, and
/// the error that stopped the rules (an invalid pattern).
public struct Selection: Sendable, Equatable {
    public var indices: [Int]
    public var trace: [SelectionTrace]
    public var requiresManualChoice: Bool
    public var error: BroMessage?

    public init(indices: [Int], trace: [SelectionTrace], requiresManualChoice: Bool, error: BroMessage?) {
        self.indices = indices
        self.trace = trace
        self.requiresManualChoice = requiresManualChoice
        self.error = error
    }
}
