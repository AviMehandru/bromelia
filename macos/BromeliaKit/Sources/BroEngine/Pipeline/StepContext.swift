import BroDomain
import BroFoundation
import BroPorts

/// Everything a step may use from its worker (plan §12.2); all of it is Sendable. The job is a snapshot: its request,
/// its plan and the outputs of the steps before this one.
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
    public var jobDirectory: String

    public init(job: JobSnapshot, cancel: CancellationToken, progress: any ProgressReporter, log: any JobLogger, decisions: any DecisionPort,
                tools: ToolSet, fs: any FileSystem, clock: any Clock, drives: any DriveControl, library: any LibraryPort,
                catalog: any CatalogQueries, jobDirectory: String) {
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
        self.jobDirectory = jobDirectory
    }
}
