import BroFoundation

/// The data of a leaseChanged event (shared/schema/internal-events.json): who holds which drive (by drive id), the
/// acquisition slots held, and the jobs holding anything, per queue.
public struct LeaseSnapshot: Sendable, Equatable {
    public var drives: [DriveHolder]
    public var acquisitions: Int
    public var jobs: JobCounts

    public init(drives: [DriveHolder], acquisitions: Int, jobs: JobCounts) {
        self.drives = drives
        self.acquisitions = acquisitions
        self.jobs = jobs
    }

    public static let empty = LeaseSnapshot(drives: [], acquisitions: 0, jobs: JobCounts(acquisition: 0, processing: 0, maintenance: 0))

    public static func json(_ snapshot: LeaseSnapshot) -> JsonValue {
        .object([
            ("drives", .array(snapshot.drives.map { .object([("drive", .string($0.drive)), ("job", .string($0.job.value))]) })),
            ("acquisitions", .integer(Int64(snapshot.acquisitions))),
            ("jobs", .object([("acquisition", .integer(Int64(snapshot.jobs.acquisition))),
                              ("processing", .integer(Int64(snapshot.jobs.processing))),
                              ("maintenance", .integer(Int64(snapshot.jobs.maintenance)))])),
        ])
    }

    /// A leaseChanged event's data, typed; nil when it isn't one.
    public static func parse(_ json: JsonValue) -> LeaseSnapshot? {
        guard let drives = json["drives"]?.array, let acquisitions = json["acquisitions"]?.int, let jobs = json["jobs"],
              let a = jobs["acquisition"]?.int, let p = jobs["processing"]?.int, let m = jobs["maintenance"]?.int else { return nil }
        var holders: [DriveHolder] = []
        for d in drives {
            guard let drive = d["drive"]?.string, let job = d["job"]?.string else { return nil }
            holders.append(DriveHolder(drive: drive, job: Id(job)))
        }
        return LeaseSnapshot(drives: holders, acquisitions: Int(acquisitions),
                             jobs: JobCounts(acquisition: Int(a), processing: Int(p), maintenance: Int(m)))
    }
}
