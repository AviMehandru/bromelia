import BroFoundation

/// A drive and the job it was given to.
public struct DriveHolder: Sendable, Equatable {
    public var drive: String
    public var job: Id

    public init(drive: String, job: Id) {
        self.drive = drive
        self.job = job
    }
}
