import BroDomain
import BroFoundation
import BroPorts

/// What SealStep wrote into the job's staging folder, for Commit: the unit's id, the record's status (success, or errors
/// after read errors), the files SHA256SUMS lists (path relative to the unit's folder, size and SHA-256 as read back from
/// disk, role, title: unit_files rows), the record's file name, the logs copied, the note (READ ERRORS.txt) and the
/// quarantine tag Commit uses ("READ ERRORS"; nil: a finished archive, committed into the library).
public struct SealOutput: Sendable, Equatable {
    public var unitId: Id
    public var status: String
    public var files: [UnitFile]
    public var record: String
    public var logs: [String]
    public var note: String?
    public var tag: String?

    public init(unitId: Id, status: String, files: [UnitFile], record: String, logs: [String], note: String?, tag: String?) {
        self.unitId = unitId
        self.status = status
        self.files = files
        self.record = record
        self.logs = logs
        self.note = note
        self.tag = tag
    }

    public static func encode(_ output: SealOutput) -> JsonValue {
        .object([("unitId", .string(output.unitId.value)), ("status", .string(output.status)),
                 ("files", .array(output.files.map(SealOutput.file))), ("record", .string(output.record)),
                 ("logs", .array(output.logs.map { .string($0) })), ("note", output.note.map { .string($0) } ?? .null),
                 ("tag", output.tag.map { .string($0) } ?? .null)])
    }

    public static func decode(_ json: JsonValue) -> SealOutput {
        SealOutput(unitId: Id(json["unitId"]?.string ?? ""), status: json["status"]?.string ?? "",
                   files: (json["files"]?.array ?? []).map { f in
                       UnitFile(path: f["path"]?.string ?? "", size: f["size"]?.int ?? 0, sha256: f["sha256"]?.string ?? "",
                                role: f["role"]?.string ?? "", title: f["title"]?.int.map { Int($0) })
                   },
                   record: json["record"]?.string ?? "", logs: (json["logs"]?.array ?? []).map { $0.string ?? "" },
                   note: json["note"]?.string, tag: json["tag"]?.string)
    }

    /// The job's Seal output; nil when Seal hasn't run.
    public static func of(_ job: JobSnapshot) -> SealOutput? {
        guard let o = JobSnapshot.output(job, .seal), o.members != nil else { return nil }
        return decode(o)
    }

    /// {path, size, sha256, role?, title?}: a file as SealOutput and the record list it.
    static func file(_ f: UnitFile) -> JsonValue {
        var members: [(key: String, value: JsonValue)] = [("path", .string(f.path)), ("size", .integer(f.size)), ("sha256", .string(f.sha256))]
        if !f.role.isEmpty { members.append(("role", .string(f.role))) }
        if let t = f.title { members.append(("title", .integer(Int64(t)))) }
        return .object(members)
    }
}
