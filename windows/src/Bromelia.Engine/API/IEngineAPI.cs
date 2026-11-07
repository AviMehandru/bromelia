using System.Collections.Generic;
using System.Threading.Tasks;
using Bromelia.Domain;
using Bromelia.Foundation;

namespace Bromelia.Engine;

/// <summary>
/// The one interface between the engine and anything outside it (plan §13.1): DTOs in and out, never live objects.
/// LocalEngine implements it now; RemoteEngine (phase 4) too. It holds the operations of the phases built so far: it
/// grows with each phase. Every operation fails with a BroFailure carrying the error the route lists; server.stopping
/// once the engine stops.
/// </summary>
public interface IEngineAPI
{
    // ---- system ----

    /// <summary>Versions, MakeMKV, tools, problems, libraries, configuration version, last event seq.</summary>
    Task<SystemStatusDto> System();

    /// <summary>The stop order: running jobs are left as a killed engine would leave them, for the next start's
    /// recovery. server.stopping when stopping already.</summary>
    Task Stop();

    // ---- drives ----

    Task<IReadOnlyList<DriveDto>> Drives();

    Task Eject(string driveId);

    Task CloseTray(string driveId);

    Task CloseAllTrays();

    /// <summary>A manual video-disc job on the drive's disc: drive.notFound, drive.noDisc, drive.busy, disc.notVideo;
    /// titles: session.notOpen until SessionService; an outputFolder that is no library's: library.notAttached.</summary>
    Task<JobDto> Rip(string driveId, RipOptionsDto options);

    // ---- jobs ----

    Task<JobPageDto> Jobs(JobQueryDto query);

    Task<JobDetailDto> Job(Id jobId);

    Task<IReadOnlyList<LogLineDto>> JobLog(Id jobId, long after);

    Task Cancel(Id jobId);

    Task<JobDto> Retry(Id jobId, StepKind? fromStep);

    /// <summary>Nothing returned (204) once queued.</summary>
    Task KeepApart(Id jobId);

    Task Decide(Id jobId, string decisionId, string answer);

    Task Move(Id jobId, int by);

    Task StartNow(Id jobId);

    // ---- libraries ----

    Task<IReadOnlyList<LibraryDto>> Libraries();

    /// <summary>Writes the folder's marker and adds the library to the configuration; if that save fails the marker is
    /// removed again and the save's error returned.</summary>
    Task<LibraryDto> AttachLibrary(string path, string name);

    // ---- configuration ----

    Task<ConfigEnvelope> Config();

    Task<ConfigEnvelope> UpdateConfig(JsonValue doc, long version);

    Task<IReadOnlyList<Issue>> ValidateConfig(JsonValue doc);

    Task SetSecret(string name, string value);

    /// <summary>The ExportBundle document.</summary>
    Task<JsonValue> ExportConfig();

    // ---- events ----

    /// <summary>The event feed from <paramref name="after"/> (null: a snapshot first); jobLog events only for the jobs
    /// in <paramref name="jobLog"/>.</summary>
    Task<EventFeed> Events(long? after, IReadOnlyCollection<Id> jobLog);
}
