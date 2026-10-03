/// An episode of a season, as listed online: its title, air date (yyyy-mm-dd or "") and plot.
public struct EpisodeDetails: Sendable, Equatable {
    public var title: String
    public var aired: String
    public var plot: String

    public init(title: String, aired: String = "", plot: String = "") {
        self.title = title
        self.aired = aired
        self.plot = plot
    }
}
