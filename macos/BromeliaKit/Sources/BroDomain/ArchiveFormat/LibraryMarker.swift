import BroFoundation
import CryptoKit

/// The .bromelia-library file at a library's root (plan §22.2): written when a library is attached (never on its own),
/// so a library whose disk isn't there is offline instead of being written to the boot disk (I2).
public struct LibraryMarker: Sendable, Equatable {
    public static let fileName = ".bromelia-library"

    public var id: String
    public var name: String
    public var created: Instant?

    public init(id: String, name: String, created: Instant?) {
        self.id = id
        self.name = name
        self.created = created
    }

    /// A marker for a library attached now. Its id is a UUID (version 8, RFC 9562) made from the first 16 bytes of
    /// SHA-256 of "<libraryId>\n<path>\n<created>": unique to this attach without any randomness.
    public static func make(_ libraryId: String, path: String, name: String, created: Instant) -> LibraryMarker {
        var b = Array(SHA256.hash(data: Array("\(libraryId)\n\(path)\n\(Instant.format(created))".utf8)).prefix(16))
        b[6] = (b[6] & 0x0F) | 0x80
        b[8] = (b[8] & 0x3F) | 0x80
        let hex = b.map { v in
            let h = String(v, radix: 16)
            return h.count == 1 ? "0" + h : h
        }.joined()
        let c = Array(hex)
        let id = [String(c[0..<8]), String(c[8..<12]), String(c[12..<16]), String(c[16..<20]), String(c[20..<32])].joined(separator: "-")
        return LibraryMarker(id: id, name: name, created: created)
    }

    /// {"id", "name", "created"}, canonical JSON with a final newline.
    public static func encode(_ marker: LibraryMarker) -> [UInt8] {
        var m: [(key: String, value: JsonValue)] = [("id", .string(marker.id)), ("name", .string(marker.name))]
        if let c = marker.created { m.append(("created", .string(Instant.format(c)))) }
        return JsonValue.encodeCanonical(.object(m))
    }

    /// Nil unless a JSON object with a string id; the name defaults to "" and created to nil.
    public static func decode(_ bytes: [UInt8]) -> LibraryMarker? {
        guard let json = JsonValue.parse(bytes), json.members != nil, let id = json["id"]?.string else { return nil }
        return LibraryMarker(id: id, name: json["name"]?.string ?? "", created: json["created"]?.string.flatMap(Instant.parse))
    }
}
