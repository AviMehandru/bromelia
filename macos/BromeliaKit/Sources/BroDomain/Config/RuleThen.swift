import BroFoundation

/// What a matching rule does: switch to a profile, merge a profile patch (an RFC 7386 merge patch), add steps after
/// the profile's.
public struct RuleThen: Sendable, Equatable {
    public var profile: String?
    public var set: JsonValue?
    public var steps: [String]

    public init(profile: String?, set: JsonValue?, steps: [String]) {
        self.profile = profile
        self.set = set
        self.steps = steps
    }
}
