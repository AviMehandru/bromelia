/// The configuration's limits, resolved (plan §21): `acquisitions` 0 = no cap; `cpu` is a count (the caller turns
/// the configuration's 0 into cores / 2: the engine can't ask the system).
public struct SchedulerLimits: Sendable, Equatable {
    public var acquisitions: Int
    public var processing: Int
    public var cpu: Int
    public var ioPerVolume: Int

    public init(acquisitions: Int = 0, processing: Int = 1, cpu: Int = 2, ioPerVolume: Int = 2) {
        self.acquisitions = acquisitions
        self.processing = processing
        self.cpu = cpu
        self.ioPerVolume = ioPerVolume
    }
}
