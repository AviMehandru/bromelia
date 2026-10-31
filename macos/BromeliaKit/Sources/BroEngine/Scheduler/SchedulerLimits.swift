import BroDomain

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

    /// The configuration's limits (config-3 Limits, its defaults where a member is missing); cpu 0 = max(1, `cores` / 2).
    /// LocalEngine applies it after the load and on every configChanged.
    public static func of(_ config: Config, cores: Int) -> SchedulerLimits {
        let limits = config.document["limits"]
        let cpu = Int(limits?["cpu"]?.int ?? 0)
        return SchedulerLimits(acquisitions: Int(limits?["acquisitions"]?.int ?? 0), processing: Int(limits?["processing"]?.int ?? 1),
                               cpu: cpu > 0 ? cpu : max(1, cores / 2), ioPerVolume: Int(limits?["ioPerVolume"]?.int ?? 2))
    }
}
