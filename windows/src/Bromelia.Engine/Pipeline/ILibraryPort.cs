using Bromelia.Foundation;

namespace Bromelia.Engine;

/// <summary>What a step may do with a library (plan §12.2), implemented by LibraryService. The commit (§22.3) joins
/// it with that service.</summary>
public interface ILibraryPort
{
    /// <summary>library.offline when its marker is missing.</summary>
    LibraryHandle Require(string libraryId);

    /// <summary>Where the job's files are made: &lt;library&gt;/.bromelia/staging/&lt;job&gt; (the same file system, so the
    /// commit is a rename).</summary>
    string StagingDirectory(string libraryId, Id jobId);
}
