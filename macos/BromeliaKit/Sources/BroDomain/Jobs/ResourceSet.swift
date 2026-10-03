/// What a step holds while it runs (plan §21): drives, an acquisition slot, library writes, CPU slots and I/O
/// slots per volume. MakeMKV launches are taken inside RegistrySwapIsolation, not here.
public struct ResourceSet: Sendable, Equatable {
    public var drives: [String]
    public var acquisition: Bool
    public var libraryWrites: [String]
    public var cpu: Int
    public var io: [String]

    public init(drives: [String] = [], acquisition: Bool = false, libraryWrites: [String] = [], cpu: Int = 0, io: [String] = []) {
        self.drives = drives
        self.acquisition = acquisition
        self.libraryWrites = libraryWrites
        self.cpu = cpu
        self.io = io
    }

    public static let none = ResourceSet()
}
