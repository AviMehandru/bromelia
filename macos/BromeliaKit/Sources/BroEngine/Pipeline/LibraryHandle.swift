/// An online library: its id and its root folder.
public struct LibraryHandle: Sendable, Equatable {
    public var id: String
    public var path: String

    public init(id: String, path: String) {
        self.id = id
        self.path = path
    }
}
