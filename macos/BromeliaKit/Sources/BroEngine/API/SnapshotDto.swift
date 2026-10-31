import BroDomain
import BroFoundation

/// The data of a snapshot event (events.json Snapshot), never published on the bus: the system, the drives, every
/// unfinished job and the last 50 finished ones, the queues as the Scheduler orders them, the problems, the libraries and
/// the configuration's version. Sessions are empty until SessionService.
public struct SnapshotDto: Sendable, Equatable {
    public var system: SystemStatusDto
    public var drives: [DriveDto]
    public var jobs: [JobDto]
    public var queues: QueueOrders
    public var problems: [ProblemDto]
    public var libraries: [LibraryDto]
    public var configVersion: Int64

    public init(system: SystemStatusDto, drives: [DriveDto], jobs: [JobDto], queues: QueueOrders, problems: [ProblemDto],
                libraries: [LibraryDto], configVersion: Int64) {
        self.system = system
        self.drives = drives
        self.jobs = jobs
        self.queues = queues
        self.problems = problems
        self.libraries = libraries
        self.configVersion = configVersion
    }

    public static func json(_ snapshot: SnapshotDto) -> JsonValue {
        .object([
            ("system", SystemStatusDto.json(snapshot.system)),
            ("drives", .array(snapshot.drives.map(DriveDto.json))),
            ("sessions", .array([])),
            ("jobs", .array(snapshot.jobs.map(JobDto.json))),
            ("queues", .object([("acquisition", ids(snapshot.queues.acquisition)), ("processing", ids(snapshot.queues.processing)),
                                ("maintenance", ids(snapshot.queues.maintenance))])),
            ("problems", .array(snapshot.problems.map(ProblemDto.json))),
            ("libraries", .array(snapshot.libraries.map(LibraryDto.json))),
            ("configVersion", .integer(snapshot.configVersion)),
        ])
    }

    private static func ids(_ ids: [Id]) -> JsonValue { .array(ids.map { .string($0.value) }) }
}
