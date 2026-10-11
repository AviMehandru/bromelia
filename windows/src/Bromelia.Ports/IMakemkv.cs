using System.Collections.Generic;
using System.Threading.Tasks;
using Bromelia.Domain;
using Bromelia.Foundation;

namespace Bromelia.Ports;

/// <summary>makemkvcon (plan §10.1): the drive scan, a disc's listing, MKV rips and backups. Every call isolates the
/// run's settings, reports each robot event to the sink and classifies the run (RunOutcome).</summary>
public interface IMakemkv
{
    /// <summary>Every drive MakeMKV sees (DRV lines).</summary>
    Task<IReadOnlyList<MakemkvDrive>> ScanDrives(CancellationToken cancel);

    Task<ListingRun> Listing(MakemkvSource source, MakemkvInvocation invocation, IRunSink sink, CancellationToken cancel);

    /// <summary>One title (its index) or "all" into the destination folder.</summary>
    Task<MakemkvRun> Rip(MakemkvSource source, string title, string destination, MakemkvInvocation invocation, IRunSink sink, CancellationToken cancel);

    /// <summary>A disc structure or an image at destination, which must not exist yet (fs.alreadyExists).</summary>
    Task<MakemkvRun> Backup(MakemkvSource source, bool decrypt, string destination, MakemkvInvocation invocation, IRunSink sink, CancellationToken cancel);
}
