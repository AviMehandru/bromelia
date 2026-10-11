/// A count per queue.
public struct JobCounts: Sendable, Equatable {
    public var acquisition: Int
    public var processing: Int
    public var maintenance: Int

    public init(acquisition: Int, processing: Int, maintenance: Int) {
        self.acquisition = acquisition
        self.processing = processing
        self.maintenance = maintenance
    }
}
