import BroFoundation

/// The jobs of each queue, in order.
public struct QueueOrders: Sendable, Equatable {
    public var acquisition: [Id]
    public var processing: [Id]
    public var maintenance: [Id]

    public init(acquisition: [Id], processing: [Id], maintenance: [Id]) {
        self.acquisition = acquisition
        self.processing = processing
        self.maintenance = maintenance
    }
}
