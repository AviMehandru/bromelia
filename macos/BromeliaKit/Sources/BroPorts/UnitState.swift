/// archive_units.state.
public enum UnitState: String, Sendable, CaseIterable {
    case committing
    case committed
    case quarantined
    case missing
}
