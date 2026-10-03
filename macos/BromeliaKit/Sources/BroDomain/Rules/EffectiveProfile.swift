import BroFoundation

/// The profile a disc gets: every field set (JSON, config-3.json's ProfileFields), the steps to run (the profile's,
/// then those rules add), and where each part came from.
public struct EffectiveProfile: Sendable, Equatable {
    public var profile: JsonValue
    public var steps: [String]
    public var trace: [ResolveTrace]

    public init(profile: JsonValue, steps: [String], trace: [ResolveTrace]) {
        self.profile = profile
        self.steps = steps
        self.trace = trace
    }
}
