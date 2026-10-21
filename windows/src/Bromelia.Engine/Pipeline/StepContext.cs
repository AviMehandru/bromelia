using Bromelia.Domain;
using Bromelia.Ports;
using CancellationToken = Bromelia.Foundation.CancellationToken;

namespace Bromelia.Engine;

/// <summary>Everything a step may use from its worker (plan §12.2); all of it is thread-safe. The job is a snapshot:
/// its request, its plan and the outputs of the steps before this one. The configuration is the one the job started
/// with. <see cref="Checkpoint"/> keeps what a resumable step saved between its attempts. <see cref="Finishing"/> is
/// set only when the runner runs the Release step at the end of a job that stopped before it (the job's outcome): the
/// step then decides whether the disc comes out. <see cref="Messages"/> gives a message's text for what a step writes
/// for people to read (Seal's log copy and notes; owner, 2026-10-19), and <see cref="AppVersion"/> is the app's version
/// for the archive record (owner, 2026-10-20).</summary>
public sealed record StepContext(
    JobSnapshot Job,
    CancellationToken Cancel,
    IProgressReporter Progress,
    IJobLogger Log,
    IDecisionPort Decisions,
    ToolSet Tools,
    IFileSystem Fs,
    IClock Clock,
    IDriveControl Drives,
    ILibraryPort Library,
    ICatalogQueries Catalog,
    Config Config,
    IDriveLookup DriveLookup,
    IMakemkvSetup MakemkvSetup,
    ISpaceReservations Space,
    ICheckpointer Checkpoint,
    string JobDirectory,
    IMessageRenderer Messages,
    string AppVersion,
    Decided? Finishing = null);
