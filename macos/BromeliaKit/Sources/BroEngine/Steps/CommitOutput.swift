import BroDomain
import BroFoundation

/// What CommitStep did: the unit, its library, its folder relative to the library's root as committed (a free name in
/// _Quarantine when quarantined), whether it was merged into a folder that is shared, and whether it was quarantined
/// (plan §15.4: the unit's id in the job's output).
public struct CommitOutput: Sendable, Equatable {
    public var unitId: Id
    public var library: String
    public var path: String
    public var merged: Bool
    public var quarantined: Bool

    public init(unitId: Id, library: String, path: String, merged: Bool, quarantined: Bool) {
        self.unitId = unitId
        self.library = library
        self.path = path
        self.merged = merged
        self.quarantined = quarantined
    }

    public static func encode(_ output: CommitOutput) -> JsonValue {
        .object([("unitId", .string(output.unitId.value)), ("library", .string(output.library)), ("path", .string(output.path)),
                 ("merged", .bool(output.merged)), ("quarantined", .bool(output.quarantined))])
    }

    public static func decode(_ json: JsonValue) -> CommitOutput {
        CommitOutput(unitId: Id(json["unitId"]?.string ?? ""), library: json["library"]?.string ?? "", path: json["path"]?.string ?? "",
                     merged: json["merged"]?.bool ?? false, quarantined: json["quarantined"]?.bool ?? false)
    }

    /// The job's Commit output; nil when Commit hasn't run.
    public static func of(_ job: JobSnapshot) -> CommitOutput? {
        guard let o = JobSnapshot.output(job, .commit), o.members != nil else { return nil }
        return decode(o)
    }
}
