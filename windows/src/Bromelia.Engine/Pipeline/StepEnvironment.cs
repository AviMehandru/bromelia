using Bromelia.Ports;

namespace Bromelia.Engine;

/// <summary>The parts of a StepContext every job shares; the composition root builds it.</summary>
/// <param name="DataDirectory">The engine's data folder: a job's folder is &lt;data&gt;/jobs/&lt;id&gt;.</param>
public sealed record StepEnvironment(
    ToolSet Tools,
    IFileSystem Fs,
    IClock Clock,
    IDriveControl Drives,
    ILibraryPort Library,
    ICatalogQueries Catalog,
    IConfigSource Config,
    IDriveLookup DriveLookup,
    IMakemkvSetup MakemkvSetup,
    string DataDirectory);
