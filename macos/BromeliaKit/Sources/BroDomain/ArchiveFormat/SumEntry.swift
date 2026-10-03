/// A line of SHA256SUMS: a path relative to the unit's folder (folders separated by `/`) and its SHA-256 in
/// lower-case hex.
public struct SumEntry: Sendable, Equatable {
    public var path: String
    public var sha256: String

    public init(path: String, sha256: String) {
        self.path = path
        self.sha256 = sha256
    }
}
