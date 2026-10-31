using System.Collections.Generic;
using System.Linq;
using Bromelia.Domain;
using Bromelia.Foundation;

namespace Bromelia.Engine;

/// <summary>The data of a snapshot event (events.json Snapshot), never published on the bus: the system, the drives,
/// every unfinished job and the last 50 finished ones, the queues as the Scheduler orders them, the problems, the
/// libraries and the configuration's version. Sessions are empty until SessionService.</summary>
public sealed record SnapshotDto(
    SystemStatusDto System,
    IReadOnlyList<DriveDto> Drives,
    IReadOnlyList<JobDto> Jobs,
    QueueOrders Queues,
    IReadOnlyList<ProblemDto> Problems,
    IReadOnlyList<LibraryDto> Libraries,
    long ConfigVersion)
{
    public static JsonValue Json(SnapshotDto snapshot) => JsonValue.Of(
        ("system", SystemStatusDto.Json(snapshot.System)),
        ("drives", JsonValue.Of(snapshot.Drives.Select(DriveDto.Json))),
        ("sessions", JsonValue.Of(new List<JsonValue>())),
        ("jobs", JsonValue.Of(snapshot.Jobs.Select(JobDto.Json))),
        ("queues", JsonValue.Of(("acquisition", Ids(snapshot.Queues.Acquisition)), ("processing", Ids(snapshot.Queues.Processing)),
            ("maintenance", Ids(snapshot.Queues.Maintenance)))),
        ("problems", JsonValue.Of(snapshot.Problems.Select(ProblemDto.Json))),
        ("libraries", JsonValue.Of(snapshot.Libraries.Select(LibraryDto.Json))),
        ("configVersion", JsonValue.Of(snapshot.ConfigVersion)));

    static JsonValue Ids(IReadOnlyList<Id> ids) => JsonValue.Of(ids.Select(id => JsonValue.Of(id.Value)));
}
