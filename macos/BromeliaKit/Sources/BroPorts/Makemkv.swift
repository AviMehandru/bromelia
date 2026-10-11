import BroDomain
import BroFoundation

/// makemkvcon (plan §10.1): the drive scan, a disc's listing, MKV rips and backups. Every call isolates the run's
/// settings, reports each robot event to the sink and classifies the run (RunOutcome).
public protocol Makemkv: Sendable {
    /// Every drive MakeMKV sees (DRV lines).
    func scanDrives(_ cancel: CancellationToken) async throws(BroError) -> [MakemkvDrive]

    func listing(_ source: MakemkvSource, invocation: MakemkvInvocation, sink: any RunSink,
                 cancel: CancellationToken) async throws(BroError) -> ListingRun

    /// One title (its index) or "all" into the destination folder.
    func rip(_ source: MakemkvSource, title: String, destination: String, invocation: MakemkvInvocation, sink: any RunSink,
             cancel: CancellationToken) async throws(BroError) -> MakemkvRun

    /// A disc structure or an image at destination, which must not exist yet (fs.alreadyExists).
    func backup(_ source: MakemkvSource, decrypt: Bool, destination: String, invocation: MakemkvInvocation, sink: any RunSink,
                cancel: CancellationToken) async throws(BroError) -> MakemkvRun
}
