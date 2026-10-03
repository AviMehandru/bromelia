/// A layer of ProfileResolver.resolve, from lowest to highest.
public enum ResolveLayer: String, Sendable, CaseIterable {
    case defaults
    case defaultProfile
    case driveProfile
    case driveOverrides
    case rule
    case session
}
