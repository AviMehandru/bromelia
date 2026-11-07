import BroDomain
import BroFoundation
import BroPorts

/// Everything a step may use from its worker (plan §12.2); all of it is Sendable. The job is a snapshot: its request,
/// its plan and the outputs of the steps before this one. The configuration is the one the job started with.
/// `checkpoint` keeps what a resumable step saved between its attempts. `finishing` is set only when the runner runs
/// the Release step at the end of a job that stopped before it (the job's outcome): the step then decides whether the
/// disc comes out. `messages` gives a message's text for what a step writes for people to read (Seal's log copy and
/// notes), and `appVersion` is the app's version for the archive record.
public struct StepContext: Sendable {
    public var job: JobSnapshot
    public var cancel: CancellationToken
    public var progress: any ProgressReporter
    public var log: any JobLogger
    public var decisions: any DecisionPort
    public var tools: ToolSet
    public var fs: any FileSystem
    public var clock: any Clock
    public var drives: any DriveControl
    public var library: any LibraryPort
    public var catalog: any CatalogQueries
    public var config: Config
    public var driveLookup: any DriveLookup
    public var makemkvSetup: any MakemkvSetup
    public var space: any SpaceReservations
    public var checkpoint: any Checkpointer
    public var jobDirectory: String
    public var messages: any MessageRenderer
    public var appVersion: String
    public var finishing: Decided?

    public init(job: JobSnapshot, cancel: CancellationToken, progress: any ProgressReporter, log: any JobLogger, decisions: any DecisionPort,
                tools: ToolSet, fs: any FileSystem, clock: any Clock, drives: any DriveControl, library: any LibraryPort,
                catalog: any CatalogQueries, config: Config, driveLookup: any DriveLookup, makemkvSetup: any MakemkvSetup,
                space: any SpaceReservations, checkpoint: any Checkpointer, jobDirectory: String, messages: any MessageRenderer,
                appVersion: String, finishing: Decided? = nil) {
        self.job = job
        self.cancel = cancel
        self.progress = progress
        self.log = log
        self.decisions = decisions
        self.tools = tools
        self.fs = fs
        self.clock = clock
        self.drives = drives
        self.library = library
        self.catalog = catalog
        self.config = config
        self.driveLookup = driveLookup
        self.makemkvSetup = makemkvSetup
        self.space = space
        self.checkpoint = checkpoint
        self.jobDirectory = jobDirectory
        self.messages = messages
        self.appVersion = appVersion
        self.finishing = finishing
    }
}
