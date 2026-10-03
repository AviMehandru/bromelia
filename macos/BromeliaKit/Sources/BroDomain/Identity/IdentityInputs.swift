/// What `Identity.resolve` works from: the listing (when there is one), the disc's label, whether a backup stays
/// encrypted, the drive's flags, a format already known (a backup's structure), the user's choices, and how many
/// episodes the menu plays in one title.
public struct IdentityInputs: Sendable, Equatable {
    public var listing: Listing?
    public var discLabel: String
    public var encrypted: Bool
    public var flags: DiscFlags?
    public var format: DiscFormat?
    public var nameOverride: String
    public var kindOverride: MediaKind?
    public var playAllEpisodes: Int

    public init(listing: Listing?, discLabel: String, encrypted: Bool, flags: DiscFlags? = nil, format: DiscFormat? = nil,
                nameOverride: String = "", kindOverride: MediaKind? = nil, playAllEpisodes: Int = 0) {
        self.listing = listing
        self.discLabel = discLabel
        self.encrypted = encrypted
        self.flags = flags
        self.format = format
        self.nameOverride = nameOverride
        self.kindOverride = kindOverride
        self.playAllEpisodes = playAllEpisodes
    }
}
