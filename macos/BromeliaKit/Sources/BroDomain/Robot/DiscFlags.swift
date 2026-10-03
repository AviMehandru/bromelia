/// The disc flags of a DRV line: which file systems MakeMKV found.
public struct DiscFlags: Sendable, Hashable {
    public var raw: Int

    public init(raw: Int) { self.raw = raw }

    public var dvdFiles: Bool { raw & 1 != 0 }
    public var hdDvdFiles: Bool { raw & 2 != 0 }
    public var blurayFiles: Bool { raw & 4 != 0 }
    public var aacsFiles: Bool { raw & 8 != 0 }
    public var bdsvmFiles: Bool { raw & 16 != 0 }

    /// MakeMKV's disc type: `Blu-ray (AACS)`, `Blu-ray`, `HD DVD`, `DVD` or `Disc`.
    public static func typeText(_ flags: DiscFlags) -> String {
        if flags.blurayFiles { return flags.aacsFiles ? "Blu-ray (AACS)" : "Blu-ray" }
        if flags.hdDvdFiles { return "HD DVD" }
        if flags.dvdFiles { return "DVD" }
        return "Disc"
    }
}
