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
}
