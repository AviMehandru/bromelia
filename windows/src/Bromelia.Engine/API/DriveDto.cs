using System.Collections.Generic;
using Bromelia.Domain;
using Bromelia.Foundation;

namespace Bromelia.Engine;

/// <summary>A drive, as clients see it (dtos.json Drive): its id, configuration entry and name, MakeMKV's
/// identification, device and index, whether it is connected, its state and disc, whether a job holds it, and what its
/// entry gives it (profile, rip on insert). The session and the countdown come with their services.</summary>
public sealed record DriveDto(
    string Id,
    string? ConfigId,
    string Name,
    string Identification,
    string Device,
    int? MakemkvIndex,
    bool Connected,
    DriveStatus State,
    MediumDto? Medium,
    bool Busy,
    Id? JobId,
    string ProfileId,
    bool RipOnInsert)
{
    /// <summary>Optional members left out when absent (makemkvIndex is null when MakeMKV hasn't numbered it).</summary>
    public static JsonValue Json(DriveDto drive)
    {
        var m = new List<KeyValuePair<string, JsonValue>> { new("id", JsonValue.Of(drive.Id)) };
        if (drive.ConfigId is { } configId) m.Add(new("configId", JsonValue.Of(configId)));
        m.Add(new("name", JsonValue.Of(drive.Name)));
        m.Add(new("identification", JsonValue.Of(drive.Identification)));
        m.Add(new("device", JsonValue.Of(drive.Device)));
        m.Add(new("makemkvIndex", drive.MakemkvIndex is { } i ? JsonValue.Of(i) : JsonValue.Null.Instance));
        m.Add(new("connected", JsonValue.Of(drive.Connected)));
        m.Add(new("state", JsonValue.Of(EnumWire.Name(drive.State))));
        if (drive.Medium is { } medium) m.Add(new("medium", MediumDto.Json(medium)));
        m.Add(new("busy", JsonValue.Of(drive.Busy)));
        if (drive.JobId is { } job) m.Add(new("jobId", JsonValue.Of(job.Value)));
        m.Add(new("profileId", JsonValue.Of(drive.ProfileId)));
        m.Add(new("ripOnInsert", JsonValue.Of(drive.RipOnInsert)));
        return new JsonValue.Object(m);
    }
}
