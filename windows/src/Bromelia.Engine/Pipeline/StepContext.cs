using Bromelia.Domain;
using Bromelia.Ports;
using CancellationToken = Bromelia.Foundation.CancellationToken;

namespace Bromelia.Engine;

/// <summary>Everything a step may use from its worker (plan §12.2); all of it is thread-safe. The job is a snapshot:
/// its request, its plan and the outputs of the steps before this one. The configuration is the one the job started
/// with.</summary>
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
    string JobDirectory);
