/// A drive as MakeMKV reports it (a DRV line): its index for `disc:N`, state, disc flags, identification (model,
/// firmware, usually serial), disc label and OS device.
public struct MakemkvDrive: Sendable, Equatable {
    public var index: Int
    public var state: DriveState
    public var flags: DiscFlags
    public var identification: String
    public var label: String
    public var device: String

    public init(index: Int, state: DriveState, flags: DiscFlags, identification: String, label: String, device: String) {
        self.index = index
        self.state = state
        self.flags = flags
        self.identification = identification
        self.label = label
        self.device = device
    }

    public static func from(_ event: RobotEvent) -> MakemkvDrive? {
        guard case let .drive(index, state, flags, identification, label, device) = event else { return nil }
        let s: DriveState
        switch state {
        case 0: s = .emptyClosed
        case 1: s = .emptyOpen
        case 2: s = .inserted
        case 3: s = .loading
        case 257: s = .unmounting
        default: s = .noDrive
        }
        return MakemkvDrive(index: index, state: s, flags: DiscFlags(raw: flags), identification: identification, label: label, device: device)
    }

    /// The drive is there: its state isn't noDrive, and it has a name or a device.
    public static func isPresent(_ drive: MakemkvDrive) -> Bool {
        drive.state != .noDrive && !(drive.identification.isEmpty && drive.device.isEmpty)
    }
}
