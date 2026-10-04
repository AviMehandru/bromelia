/// What moveMerging does when the destination exists: never replace (ConflictNamer picks another name).
public enum MovePolicy: String, Sendable, CaseIterable {
    case neverReplace
}
