/// What rules are matched against: the movie or show name, the disc label, the format code, movie or TV, the drive
/// entry's id, the profile the disc got before rules, and whether the rip is automatic.
public struct RuleFacts: Sendable, Equatable {
    public var name: String
    public var label: String
    public var formatCode: String
    public var kind: String?
    public var driveId: String?
    public var profileId: String?
    public var automatic: Bool

    public init(name: String, label: String, formatCode: String, kind: String? = nil, driveId: String? = nil, profileId: String? = nil,
                automatic: Bool = false) {
        self.name = name
        self.label = label
        self.formatCode = formatCode
        self.kind = kind
        self.driveId = driveId
        self.profileId = profileId
        self.automatic = automatic
    }
}
