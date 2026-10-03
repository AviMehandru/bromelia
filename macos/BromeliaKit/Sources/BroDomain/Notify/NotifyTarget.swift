/// A notification target of the configuration; its URL is the secret named `secret` (it carries credentials),
/// resolved by the keystore.
public struct NotifyTarget: Sendable, Equatable {
    public var id: String
    public var name: String
    public var secret: String
    public var enabled: Bool
    public var onlyProblems: Bool

    public init(id: String, secret: String, name: String = "", enabled: Bool = true, onlyProblems: Bool = false) {
        self.id = id
        self.name = name
        self.secret = secret
        self.enabled = enabled
        self.onlyProblems = onlyProblems
    }
}
