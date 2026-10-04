import BroDomain

/// What an encode came to: HandBrakeCLI's exit, and whether the step's timeout stopped it.
public struct HandBrakeRun: Sendable, Equatable {
    public var exit: ProcessExit
    public var timedOut: Bool

    public init(exit: ProcessExit, timedOut: Bool) {
        self.exit = exit
        self.timedOut = timedOut
    }
}
