/// A UUID in lower-case text. Ids are made by the engine's adapters (randomness is I/O).
public struct Id: Sendable, Hashable, CustomStringConvertible {
    public var value: String

    public init(_ value: String) { self.value = value }

    /// The first eight hex digits, as used in file names (`bromelia-<unit8>.json`).
    public static func short(_ id: Id) -> String {
        String(id.value.filter { $0 != "-" }.lowercased().prefix(8))
    }

    public var description: String { value }
}
