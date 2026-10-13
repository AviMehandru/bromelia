import BroDomain
import BroFoundation

/// A row of libraries: the config entry's id, the id in its .bromelia-library marker, whether the marker was there at
/// the last check and when it was last seen.
public struct LibraryRecord: Sendable, Equatable {
    /// The config entry's id.
    public var id: String
    public var name: String
    public var path: String
    public var markerId: String
    public var attachedAt: Instant
    public var online: Bool
    public var lastSeenAt: Instant?

    public init(id: String, name: String, path: String, markerId: String, attachedAt: Instant, online: Bool, lastSeenAt: Instant? = nil) {
        self.id = id
        self.name = name
        self.path = path
        self.markerId = markerId
        self.attachedAt = attachedAt
        self.online = online
        self.lastSeenAt = lastSeenAt
    }
}
