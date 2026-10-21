import BroDomain
import BroFoundation

/// What ProbeStep found: the listing (none when a backup goes on without one), its fingerprint, the LibreDrive detail,
/// whether the listing is the one the disc was opened with (a backup whose listing failed), and MakeMKV's version line
/// (message 1005; nil when the run didn't print one), which Seal writes into the archive record.
public struct ProbeOutput: Sendable, Equatable {
    public var listing: Listing?
    public var fingerprint: String?
    public var libreDrive: String?
    public var fromSession: Bool
    public var makemkvVersion: String?

    public init(listing: Listing?, fingerprint: String?, libreDrive: String?, fromSession: Bool, makemkvVersion: String? = nil) {
        self.listing = listing
        self.fingerprint = fingerprint
        self.libreDrive = libreDrive
        self.fromSession = fromSession
        self.makemkvVersion = makemkvVersion
    }

    public static func encode(_ output: ProbeOutput) -> JsonValue {
        .object([("listing", output.listing.map(Listing.encode) ?? .null), ("fingerprint", output.fingerprint.map { .string($0) } ?? .null),
                 ("libreDrive", output.libreDrive.map { .string($0) } ?? .null), ("fromSession", .bool(output.fromSession)),
                 ("makemkvVersion", output.makemkvVersion.map { .string($0) } ?? .null)])
    }

    public static func decode(_ json: JsonValue) -> ProbeOutput {
        var listing: Listing?
        if let l = json["listing"], l.members != nil { listing = Listing.decode(l) }
        return ProbeOutput(listing: listing, fingerprint: json["fingerprint"]?.string, libreDrive: json["libreDrive"]?.string,
                           fromSession: json["fromSession"]?.bool ?? false, makemkvVersion: json["makemkvVersion"]?.string)
    }
}
