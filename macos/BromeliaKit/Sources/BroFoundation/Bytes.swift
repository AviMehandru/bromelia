/// A number of bytes.
public struct Bytes: Sendable, Hashable, Comparable {
    public var count: Int64

    public init(_ count: Int64) { self.count = count }

    public static func < (a: Bytes, b: Bytes) -> Bool { a.count < b.count }
}
