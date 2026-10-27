using System.Collections.Generic;
using System.Linq;
using Bromelia.Domain;
using Bromelia.Foundation;

namespace Bromelia.Engine;

/// <summary>A job with its steps, files and commands (dtos.json JobDetail: the Job's members, then these). Children are
/// left out until JobQuery can ask for a job's children (decision 171).</summary>
public sealed record JobDetailDto(JobDto Job, IReadOnlyList<JobStepDto> Steps, IReadOnlyList<JobFileDto> Files, IReadOnlyList<string> Commands,
    long LogBytes)
{
    /// <summary>The Job's members, then steps, files, commands and logBytes; optional members left out when absent.</summary>
    public static JsonValue Json(JobDetailDto detail)
    {
        var m = new List<KeyValuePair<string, JsonValue>>(((JsonValue.Object)JobDto.Json(detail.Job)).Members)
        {
            new("steps", JsonValue.Of(detail.Steps.Select(StepJson))),
            new("files", JsonValue.Of(detail.Files.Select(FileJson))),
            new("commands", JsonValue.Of(detail.Commands.Select(c => JsonValue.Of(c)))),
            new("logBytes", JsonValue.Of(detail.LogBytes)),
        };
        return new JsonValue.Object(m);
    }

    static JsonValue StepJson(JobStepDto step)
    {
        var m = new List<KeyValuePair<string, JsonValue>>
        {
            new("seq", JsonValue.Of(step.Seq)),
            new("kind", JsonValue.Of(EnumWire.Name(step.Kind))),
            new("state", JsonValue.Of(EnumWire.Name(step.State))),
        };
        if (step.StartedAt is { } started) m.Add(new("startedAt", JsonValue.Of(Instant.Format(started))));
        if (step.FinishedAt is { } finished) m.Add(new("finishedAt", JsonValue.Of(Instant.Format(finished))));
        if (step.Error is { } error) m.Add(new("error", JsonDto.Error(error)));
        if (step.Summary is { } summary) m.Add(new("summary", summary.ToJson()));
        return new JsonValue.Object(m);
    }

    static JsonValue FileJson(JobFileDto file)
    {
        var m = new List<KeyValuePair<string, JsonValue>> { new("path", JsonValue.Of(file.Path)) };
        if (file.Size is { } size) m.Add(new("size", JsonValue.Of(size)));
        if (file.Sha256 is { } sha256) m.Add(new("sha256", JsonValue.Of(sha256)));
        if (file.Role is { } role) m.Add(new("role", JsonValue.Of(role)));
        return new JsonValue.Object(m);
    }
}
