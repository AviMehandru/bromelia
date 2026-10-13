import BroDomain
import BroFoundation

/// A library, as clients see it (dtos.json Library): online, or the reason it isn't; the free space when it is; when
/// its marker was last seen.
public struct LibraryDto: Sendable, Equatable {
    public var id: String
    public var name: String
    public var path: String
    public var online: Bool
    public var reason: BroMessage?
    public var freeBytes: Int64?
    public var lastSeenAt: Instant?

    public init(id: String, name: String, path: String, online: Bool, reason: BroMessage?, freeBytes: Int64?, lastSeenAt: Instant?) {
        self.id = id
        self.name = name
        self.path = path
        self.online = online
        self.reason = reason
        self.freeBytes = freeBytes
        self.lastSeenAt = lastSeenAt
    }

    /// Optional members left out when absent.
    public static func json(_ library: LibraryDto) -> JsonValue {
        var m: [(key: String, value: JsonValue)] = [("id", .string(library.id)), ("name", .string(library.name)), ("path", .string(library.path)),
                                                    ("online", .bool(library.online))]
        if let r = library.reason { m.append(("reason", .object((r.toJson().members ?? []) + [("severity", .string(r.severity.rawValue))]))) }
        if let free = library.freeBytes { m.append(("freeBytes", .integer(free))) }
        if let seen = library.lastSeenAt { m.append(("lastSeenAt", .string(Instant.format(seen)))) }
        return .object(m)
    }
}
