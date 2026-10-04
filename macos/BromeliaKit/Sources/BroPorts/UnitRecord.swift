import BroDomain
import BroFoundation

/// A row of archive_units: one archived acquisition of a disc.
public struct UnitRecord: Sendable, Equatable {
    public var id: Id
    public var libraryId: String
    public var physicalDiscId: Id?
    public var jobId: Id?
    /// Relative to the library.
    public var path: String
    public var recordFile: String
    public var recordVersion: Int
    public var state: UnitState
    public var status: UnitStatus
    public var name: String
    public var kind: MediaKind
    public var format: String
    public var formatCode: String
    public var encrypted: Bool
    public var fingerprint: String?
    public var label: String
    public var season: Int?
    public var part: Int?
    public var volume: Int?
    public var disc: Int?
    public var makemkvVersion: String
    public var bytes: Int64
    public var fileCount: Int
    public var attempts: Int
    public var createdAt: Instant
    public var committedAt: Instant?

    public init(id: Id, libraryId: String, physicalDiscId: Id? = nil, jobId: Id? = nil, path: String,
                recordFile: String, recordVersion: Int, state: UnitState, status: UnitStatus, name: String,
                kind: MediaKind, format: String, formatCode: String, encrypted: Bool, fingerprint: String? = nil,
                label: String, season: Int? = nil, part: Int? = nil, volume: Int? = nil, disc: Int? = nil,
                makemkvVersion: String, bytes: Int64, fileCount: Int, attempts: Int, createdAt: Instant,
                committedAt: Instant? = nil) {
        self.id = id
        self.libraryId = libraryId
        self.physicalDiscId = physicalDiscId
        self.jobId = jobId
        self.path = path
        self.recordFile = recordFile
        self.recordVersion = recordVersion
        self.state = state
        self.status = status
        self.name = name
        self.kind = kind
        self.format = format
        self.formatCode = formatCode
        self.encrypted = encrypted
        self.fingerprint = fingerprint
        self.label = label
        self.season = season
        self.part = part
        self.volume = volume
        self.disc = disc
        self.makemkvVersion = makemkvVersion
        self.bytes = bytes
        self.fileCount = fileCount
        self.attempts = attempts
        self.createdAt = createdAt
        self.committedAt = committedAt
    }
}
