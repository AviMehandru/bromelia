/// Where a secret's value can come from, in the order they are asked (config secrets.prefer).
public enum SecretSource: String, Sendable, CaseIterable {
    case keystore
    case environment
    case file
}
