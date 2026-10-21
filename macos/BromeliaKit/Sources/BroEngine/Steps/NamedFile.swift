import BroFoundation

/// An item a job made, by its final name: a file or a folder (a backup) relative to the job's staging folder, which is
/// the unit's folder as it will be in the library; its role in the archive record (title, episode, closingClip, playAll,
/// backup, image, audio, discInfo; nil for a file no title owns) and the title it is of.
public struct NamedFile: Sendable, Equatable {
    public var path: String
    public var role: String?
    public var title: Int?

    public init(path: String, role: String?, title: Int?) {
        self.path = path
        self.role = role
        self.title = title
    }

    public static func encode(_ file: NamedFile) -> JsonValue {
        var members: [(key: String, value: JsonValue)] = [("path", .string(file.path))]
        if let role = file.role { members.append(("role", .string(role))) }
        if let t = file.title { members.append(("title", .integer(Int64(t)))) }
        return .object(members)
    }

    public static func decode(_ json: JsonValue) -> NamedFile {
        NamedFile(path: json["path"]?.string ?? "", role: json["role"]?.string, title: json["title"]?.int.map { Int($0) })
    }
}
