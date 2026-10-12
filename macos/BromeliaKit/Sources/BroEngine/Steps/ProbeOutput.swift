import BroDomain
import BroFoundation

/// What ProbeStep found: the listing (none when a backup goes on without one), its fingerprint, the LibreDrive detail,
/// and whether the listing is the one the disc was opened with (a backup whose listing failed).
public struct ProbeOutput: Sendable, Equatable {
    public var listing: Listing?
    public var fingerprint: String?
    public var libreDrive: String?
    public var fromSession: Bool

    public init(listing: Listing?, fingerprint: String?, libreDrive: String?, fromSession: Bool) {
        self.listing = listing
        self.fingerprint = fingerprint
        self.libreDrive = libreDrive
        self.fromSession = fromSession
    }

    public static func encode(_ output: ProbeOutput) -> JsonValue {
        .object([("listing", output.listing.map(Listing.encode) ?? .null), ("fingerprint", output.fingerprint.map { .string($0) } ?? .null),
                 ("libreDrive", output.libreDrive.map { .string($0) } ?? .null), ("fromSession", .bool(output.fromSession))])
    }

    public static func decode(_ json: JsonValue) -> ProbeOutput {
        var listing: Listing?
        if let l = json["listing"], l.members != nil { listing = Listing.decode(l) }
        return ProbeOutput(listing: listing, fingerprint: json["fingerprint"]?.string, libreDrive: json["libreDrive"]?.string,
                           fromSession: json["fromSession"]?.bool ?? false)
    }
}
