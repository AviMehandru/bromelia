import BroDomain
import BroFoundation

/// What AcquireStep left in the job's staging folder, by name (plan §12.4; the steps after it use these lists, never a
/// count): the titles in plan order, the files of a run no title owns (a file an "all" run made that the listing
/// doesn't name, and what a failed run left), the backup (a folder or an image) when it is kept, disc-info.json when it
/// was written, and the read errors of runs that ripped several titles at once or made the backup.
public struct AcquireOutput: Sendable, Equatable {
    public var titles: [AcquiredTitle]
    public var others: [String]
    public var backup: String?
    public var discInfo: String?
    public var readErrors: [String]

    public init(titles: [AcquiredTitle], others: [String], backup: String?, discInfo: String?, readErrors: [String]) {
        self.titles = titles
        self.others = others
        self.backup = backup
        self.discInfo = discInfo
        self.readErrors = readErrors
    }

    public static func encode(_ output: AcquireOutput) -> JsonValue {
        .object([("titles", .array(output.titles.map(AcquiredTitle.encode))), ("others", .array(output.others.map { .string($0) })),
                 ("backup", output.backup.map { .string($0) } ?? .null), ("discInfo", output.discInfo.map { .string($0) } ?? .null),
                 ("readErrors", .array(output.readErrors.map { .string($0) }))])
    }

    public static func decode(_ json: JsonValue) -> AcquireOutput {
        AcquireOutput(titles: (json["titles"]?.array ?? []).map(AcquiredTitle.decode), others: (json["others"]?.array ?? []).map { $0.string ?? "" },
                      backup: json["backup"]?.string, discInfo: json["discInfo"]?.string,
                      readErrors: (json["readErrors"]?.array ?? []).map { $0.string ?? "" })
    }

    /// The job's Acquire output; nil when Acquire hasn't run.
    public static func of(_ job: JobSnapshot) -> AcquireOutput? {
        guard let o = JobSnapshot.output(job, .acquire), o.members != nil else { return nil }
        return decode(o)
    }

    /// Every name Acquire made in staging: the titles' files, the others, the backup and disc-info.json.
    public static func produced(_ output: AcquireOutput) -> [String] {
        output.titles.flatMap(\.files) + output.others + [output.backup, output.discInfo].compactMap { $0 }
    }

    /// How many read errors MakeMKV reported in all.
    public static func readErrorCount(_ output: AcquireOutput) -> Int {
        output.titles.reduce(0) { $0 + $1.readErrors.count } + output.readErrors.count
    }
}
