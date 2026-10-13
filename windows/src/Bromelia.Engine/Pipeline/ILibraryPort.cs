using System.Collections.Generic;
using System.Threading.Tasks;
using Bromelia.Foundation;
using Bromelia.Ports;

namespace Bromelia.Engine;

/// <summary>What a step may do with a library (plan §12.2), implemented by LibraryService.</summary>
public interface ILibraryPort
{
    /// <summary>library.notFound, or the reason it is offline (library.offline when its marker is missing).</summary>
    LibraryHandle Require(string libraryId);

    /// <summary>Where the job's files are made: &lt;library&gt;/.bromelia/staging/&lt;job&gt; (the same file system, so the
    /// commit is a rename), made when it isn't there; never the library's root.</summary>
    string StagingDirectory(string libraryId, Id jobId);

    /// <summary>Plan §22.3 steps 4–6 (Seal did 1–3): the intent, the move (a new folder renamed whole; a shared one
    /// item by item, SHA256SUMS merged), the folders synced, then the unit committed with its files. Any thread but the
    /// executor.</summary>
    Task<CommitResult> Commit(UnitRecord unit, CommitIntent intent, IReadOnlyList<UnitFile> files);

    /// <summary><see cref="Commit"/> into &lt;library&gt;/_Quarantine/&lt;name&gt; [&lt;tag&gt;] (a free name), the unit
    /// quarantined.</summary>
    Task<CommitResult> Quarantine(UnitRecord unit, CommitIntent intent, IReadOnlyList<UnitFile> files, string tag);
}
