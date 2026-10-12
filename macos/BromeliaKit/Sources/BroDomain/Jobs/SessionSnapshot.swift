/// An opened disc as it was when the user ripped it: the listing it was opened with and the choices made on it
/// (plan §15.3). ReconcileStep checks the disc is still that disc.
public struct SessionSnapshot: Sendable, Equatable {
    public var listing: Listing
    public var choices: SessionChoices

    public init(listing: Listing, choices: SessionChoices) {
        self.listing = listing
        self.choices = choices
    }
}
