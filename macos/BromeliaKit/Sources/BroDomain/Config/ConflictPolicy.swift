/// When the unit's folder exists: a new folder "Name (2)", merge item by item (never replacing), or fail.
public enum ConflictPolicy: String, Sendable, CaseIterable {
    case newFolder
    case merge
    case skip
}
