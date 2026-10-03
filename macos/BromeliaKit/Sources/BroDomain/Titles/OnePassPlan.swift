/// Rip the chosen titles in one makemkvcon run, with this minimum title length (seconds).
public struct OnePassPlan: Sendable, Equatable {
    public var minLength: Int

    public init(minLength: Int) { self.minLength = minLength }
}
