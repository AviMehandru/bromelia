import BroDomain
import BroFoundation

/// An entry of a folder.
public struct DirectoryEntry: Sendable, Equatable {
    public var name: String
    public var isDirectory: Bool

    public init(name: String, isDirectory: Bool) {
        self.name = name
        self.isDirectory = isDirectory
    }
}
