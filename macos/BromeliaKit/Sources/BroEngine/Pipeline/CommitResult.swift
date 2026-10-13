import BroFoundation
import BroPorts

/// What a commit did: the unit, its folder (absolute), its files as committed, and whether they were added to an
/// existing folder.
public struct CommitResult: Sendable, Equatable {
    public var unitId: Id
    public var destination: String
    public var files: [UnitFile]
    public var merged: Bool

    public init(unitId: Id, destination: String, files: [UnitFile], merged: Bool) {
        self.unitId = unitId
        self.destination = destination
        self.files = files
        self.merged = merged
    }
}
