import BroDomain

/// A connected drive as DriveService knows it: MakeMKV's index (nil until a scan saw it), the OS device ("" when
/// unknown, as on macOS for an empty drive), the configuration entry that matches it, the generation of its disc (nil:
/// no disc), the disc's label and MakeMKV's flags for it.
public struct DriveLocation: Sendable, Equatable {
    public var makemkvIndex: Int?
    public var device: String
    public var configId: String?
    public var generation: Int64?
    public var label: String
    public var flags: DiscFlags?

    public init(makemkvIndex: Int?, device: String, configId: String?, generation: Int64?, label: String = "", flags: DiscFlags? = nil) {
        self.makemkvIndex = makemkvIndex
        self.device = device
        self.configId = configId
        self.generation = generation
        self.label = label
        self.flags = flags
    }
}
