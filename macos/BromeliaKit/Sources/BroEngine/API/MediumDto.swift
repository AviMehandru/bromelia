import BroDomain
import BroFoundation

/// The disc in a drive (dtos.json Medium): its generation (the drive's discs counted since the engine started), label,
/// type (dvd, bd, hddvd or disc, from MakeMKV's flags), MakeMKV's type text, flags, what is on it and where the OS
/// mounted it.
public struct MediumDto: Sendable, Equatable {
    public var generation: Int64
    public var label: String
    public var type: String
    public var typeText: String
    public var flags: [String]
    public var content: DiscContent
    public var mountPath: String?

    public init(generation: Int64, label: String, type: String, typeText: String, flags: [String], content: DiscContent, mountPath: String?) {
        self.generation = generation
        self.label = label
        self.type = type
        self.typeText = typeText
        self.flags = flags
        self.content = content
        self.mountPath = mountPath
    }

    /// The flags' names, in dtos.json order.
    public static func flagNames(_ flags: DiscFlags) -> [String] {
        var names: [String] = []
        if flags.dvdFiles { names.append("dvdFiles") }
        if flags.hdDvdFiles { names.append("hdDvdFiles") }
        if flags.blurayFiles { names.append("blurayFiles") }
        if flags.aacsFiles { names.append("aacsFiles") }
        if flags.bdsvmFiles { names.append("bdsvmFiles") }
        return names
    }

    /// The {type} of a disc with these flags (common.json DiscType).
    public static func typeOf(_ flags: DiscFlags) -> String {
        switch FormatDetector.detect(nil, flags: flags, indexBdmv: nil) {
        case .bluray, .uhd: return "bd"
        case .hddvd: return "hddvd"
        case .dvd: return "dvd"
        case .unknown: return "disc"
        }
    }

    public static func json(_ medium: MediumDto) -> JsonValue {
        .object([("generation", .integer(medium.generation)), ("label", .string(medium.label)), ("type", .string(medium.type)),
                 ("typeText", .string(medium.typeText)), ("flags", .array(medium.flags.map { .string($0) })),
                 ("content", .string(medium.content.rawValue)), ("mountPath", medium.mountPath.map { .string($0) } ?? .null)])
    }
}
