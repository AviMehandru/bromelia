/// A secret the configuration refers to, and whether it has a value (never the value).
public struct SecretStatus: Sendable, Equatable {
    public var name: String
    public var set: Bool

    public init(name: String, set: Bool) {
        self.name = name
        self.set = set
    }
}
