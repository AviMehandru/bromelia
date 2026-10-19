import BroFoundation

/// A title AcquireStep ripped: its index as the plan numbers it (the listing Probe read), the files it made in the job's
/// staging folder (MakeMKV's names), and what MakeMKV said about read errors while ripping it.
public struct AcquiredTitle: Sendable, Equatable {
    public var index: Int
    public var files: [String]
    public var readErrors: [String]

    public init(index: Int, files: [String], readErrors: [String]) {
        self.index = index
        self.files = files
        self.readErrors = readErrors
    }

    public static func encode(_ title: AcquiredTitle) -> JsonValue {
        .object([("index", .integer(Int64(title.index))), ("files", .array(title.files.map { .string($0) })),
                 ("readErrors", .array(title.readErrors.map { .string($0) }))])
    }

    public static func decode(_ json: JsonValue) -> AcquiredTitle {
        AcquiredTitle(index: Int(json["index"]?.int ?? 0), files: (json["files"]?.array ?? []).map { $0.string ?? "" },
                      readErrors: (json["readErrors"]?.array ?? []).map { $0.string ?? "" })
    }
}
