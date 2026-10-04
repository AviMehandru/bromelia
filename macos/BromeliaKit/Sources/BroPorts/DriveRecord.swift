import BroDomain
import BroFoundation

/// A row of drives.
public struct DriveRecord: Sendable, Equatable {
    /// DriveId.
    public var id: String
    public var identification: String
    public var model: String
    public var lastDevice: String
    public var configId: String?
    public var firstSeenAt: Instant
    public var lastSeenAt: Instant

    public init(id: String, identification: String, model: String, lastDevice: String, configId: String? = nil,
                firstSeenAt: Instant, lastSeenAt: Instant) {
        self.id = id
        self.identification = identification
        self.model = model
        self.lastDevice = lastDevice
        self.configId = configId
        self.firstSeenAt = firstSeenAt
        self.lastSeenAt = lastSeenAt
    }
}
