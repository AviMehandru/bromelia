/// A rule's conditions; every condition given must hold, and an empty one matches every disc: regular expressions on
/// the name and label, format codes (a trailing * matches a prefix), kinds, drive entries, the profile before rules,
/// and automatic or not.
public struct RuleWhen: Sendable, Equatable {
    public var nameOrLabel: String?
    public var name: String?
    public var label: String?
    public var formats: [String]?
    public var kinds: [String]?
    public var drives: [String]?
    public var profiles: [String]?
    public var automatic: Bool?

    public init(nameOrLabel: String? = nil, name: String? = nil, label: String? = nil, formats: [String]? = nil, kinds: [String]? = nil,
                drives: [String]? = nil, profiles: [String]? = nil, automatic: Bool? = nil) {
        self.nameOrLabel = nameOrLabel
        self.name = name
        self.label = label
        self.formats = formats
        self.kinds = kinds
        self.drives = drives
        self.profiles = profiles
        self.automatic = automatic
    }
}
