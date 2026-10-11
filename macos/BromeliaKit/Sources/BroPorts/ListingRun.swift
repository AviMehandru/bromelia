import BroDomain

/// An info run: the listing it printed and the run.
public struct ListingRun: Sendable, Equatable {
    public var listing: Listing
    public var run: MakemkvRun

    public init(listing: Listing, run: MakemkvRun) {
        self.listing = listing
        self.run = run
    }
}
