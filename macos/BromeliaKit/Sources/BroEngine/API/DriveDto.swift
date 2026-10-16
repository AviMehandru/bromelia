import BroDomain
import BroFoundation

/// A drive, as clients see it (dtos.json Drive): its id, configuration entry and name, MakeMKV's identification, device
/// and index, whether it is connected, its state and disc, whether a job holds it, and what its entry gives it (profile,
/// rip on insert). The session and the countdown come with their services.
public struct DriveDto: Sendable, Equatable {
    public var id: String
    public var configId: String?
    public var name: String
    public var identification: String
    public var device: String
    public var makemkvIndex: Int?
    public var connected: Bool
    public var state: DriveStatus
    public var medium: MediumDto?
    public var busy: Bool
    public var jobId: Id?
    public var profileId: String
    public var ripOnInsert: Bool

    public init(id: String, configId: String?, name: String, identification: String, device: String, makemkvIndex: Int?, connected: Bool,
                state: DriveStatus, medium: MediumDto?, busy: Bool, jobId: Id?, profileId: String, ripOnInsert: Bool) {
        self.id = id
        self.configId = configId
        self.name = name
        self.identification = identification
        self.device = device
        self.makemkvIndex = makemkvIndex
        self.connected = connected
        self.state = state
        self.medium = medium
        self.busy = busy
        self.jobId = jobId
        self.profileId = profileId
        self.ripOnInsert = ripOnInsert
    }

    /// Optional members left out when absent (makemkvIndex is null when MakeMKV hasn't numbered it).
    public static func json(_ drive: DriveDto) -> JsonValue {
        var m: [(key: String, value: JsonValue)] = [("id", .string(drive.id))]
        if let configId = drive.configId { m.append(("configId", .string(configId))) }
        m.append(("name", .string(drive.name)))
        m.append(("identification", .string(drive.identification)))
        m.append(("device", .string(drive.device)))
        m.append(("makemkvIndex", drive.makemkvIndex.map { .integer(Int64($0)) } ?? .null))
        m.append(("connected", .bool(drive.connected)))
        m.append(("state", .string(drive.state.rawValue)))
        if let medium = drive.medium { m.append(("medium", MediumDto.json(medium))) }
        m.append(("busy", .bool(drive.busy)))
        if let job = drive.jobId { m.append(("jobId", .string(job.value))) }
        m.append(("profileId", .string(drive.profileId)))
        m.append(("ripOnInsert", .bool(drive.ripOnInsert)))
        return .object(m)
    }
}
