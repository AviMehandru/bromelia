import BroFoundation

/// A library of the configuration (config-3.json's `Library`): where archives are committed.
public struct LibraryEntry: Sendable, Equatable {
    public var id: String
    public var name: String
    public var path: String

    public init(id: String, name: String, path: String) {
        self.id = id
        self.name = name
        self.path = path
    }

    /// A library entry from its JSON ("" for what it leaves out).
    public static func decode(_ json: JsonValue) -> LibraryEntry {
        LibraryEntry(id: json["id"]?.string ?? "", name: json["name"]?.string ?? "", path: json["path"]?.string ?? "")
    }
}
