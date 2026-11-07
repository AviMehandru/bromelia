import BroDomain
import BroFoundation

/// What NameStep decided (Seal reads it): the unit's folder relative to the library root (Layouts.paths,
/// ConflictNamer), its name (a quarantined unit goes to _Quarantine/<name> [<tag>]), whether the folder is shared (a
/// media server layout: the commit merges item by item), every produced item by the name Name gave it in staging, and
/// the identity when Name corrected it (the format a backup showed; nil: Plan's stands, and the key is left out). Name
/// renames inside staging, so staging is laid out as the unit's folder will be.
public struct NameOutput: Sendable, Equatable {
    public var folder: String
    public var name: String
    public var merge: Bool
    public var files: [NamedFile]
    public var identity: Identity?

    public init(folder: String, name: String, merge: Bool, files: [NamedFile], identity: Identity? = nil) {
        self.folder = folder
        self.name = name
        self.merge = merge
        self.files = files
        self.identity = identity
    }

    public static func encode(_ output: NameOutput) -> JsonValue {
        var members: [(key: String, value: JsonValue)] = [("folder", .string(output.folder)), ("name", .string(output.name)),
                                                          ("merge", .bool(output.merge)), ("files", .array(output.files.map(NamedFile.encode)))]
        if let identity = output.identity { members.append(("identity", Identity.encode(identity))) }
        return .object(members)
    }

    public static func decode(_ json: JsonValue) -> NameOutput {
        NameOutput(folder: json["folder"]?.string ?? "", name: json["name"]?.string ?? "", merge: json["merge"]?.bool ?? false,
                   files: (json["files"]?.array ?? []).map(NamedFile.decode),
                   identity: json["identity"].flatMap { $0.members != nil ? Identity.decode($0) : nil })
    }

    /// The job's Name output; nil when Name hasn't run.
    public static func of(_ job: JobSnapshot) -> NameOutput? {
        guard let o = JobSnapshot.output(job, .name), o.members != nil else { return nil }
        return decode(o)
    }

    /// The produced items as Acquire left them, when no Name step ran: each title's files (role title), the files no
    /// title owns (no role), the backup (role backup) and disc-info.json (role discInfo).
    public static func fromAcquire(_ acquired: AcquireOutput) -> [NamedFile] {
        acquired.titles.flatMap { t in t.files.map { NamedFile(path: $0, role: "title", title: t.index) } }
            + acquired.others.map { NamedFile(path: $0, role: nil, title: nil) }
            + (acquired.backup.map { [NamedFile(path: $0, role: "backup", title: nil)] } ?? [])
            + (acquired.discInfo.map { [NamedFile(path: $0, role: "discInfo", title: nil)] } ?? [])
    }
}
