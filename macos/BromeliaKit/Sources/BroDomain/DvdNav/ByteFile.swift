/// A file of a ByteSource: its upper-case name and size in bytes.
public struct ByteFile: Sendable, Equatable {
    public var name: String
    public var size: Int64

    public init(name: String, size: Int64) {
        self.name = name
        self.size = size
    }
}
