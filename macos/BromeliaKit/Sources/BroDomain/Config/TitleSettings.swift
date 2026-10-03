import BroFoundation

/// A profile's title rules (config-3.json's `TitleRules` object; `TitleRules` applies them): filters (duration,
/// chapters, size, patterns, angles), duplicate removal, the strategy, then maxTitles. The defaults are
/// config-3.json's.
public struct TitleSettings: Sendable, Equatable {
    public var strategy: TitleStrategy = .all
    public var longestCount = 1
    public var indexPattern = ""
    public var indexBase: IndexBase = .makemkv
    public var minDurationSeconds = 0
    public var maxDurationSeconds = 0
    public var minChapters = 0
    public var maxChapters = 0
    public var minSizeMB = 0
    public var maxSizeMB = 0
    public var includePattern = ""
    public var excludePattern = ""
    public var skipDuplicates = true
    public var skipAlternateAngles = false
    public var maxTitles = 0

    public init() {}

    /// Title rules from their JSON (config-3.json's TitleRules), with defaults for what it leaves out.
    public static func decode(_ json: JsonValue) -> TitleSettings {
        var issues: [Issue] = []
        let j = SchemaWalker.normalize(json, SchemaWalker.def("TitleRules"), "", fill: true, &issues)
        func i(_ k: String) -> Int { Int(j[k]?.int ?? 0) }
        var t = TitleSettings()
        t.strategy = TitleStrategy(rawValue: j["strategy"]?.string ?? "") ?? .all
        t.longestCount = i("longestCount")
        t.indexPattern = j["indexPattern"]?.string ?? ""
        t.indexBase = IndexBase(rawValue: j["indexBase"]?.string ?? "") ?? .makemkv
        t.minDurationSeconds = i("minDurationSeconds")
        t.maxDurationSeconds = i("maxDurationSeconds")
        t.minChapters = i("minChapters")
        t.maxChapters = i("maxChapters")
        t.minSizeMB = i("minSizeMB")
        t.maxSizeMB = i("maxSizeMB")
        t.includePattern = j["includePattern"]?.string ?? ""
        t.excludePattern = j["excludePattern"]?.string ?? ""
        t.skipDuplicates = j["skipDuplicates"]?.bool ?? true
        t.skipAlternateAngles = j["skipAlternateAngles"]?.bool ?? false
        t.maxTitles = i("maxTitles")
        return t
    }
}
