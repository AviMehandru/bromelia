using System.Collections.Generic;
using Bromelia.Domain;
using Bromelia.Foundation;

namespace Bromelia.Engine;

/// <summary>A running job's progress (dtos.json Progress): <see cref="Fraction"/> of the whole job,
/// <see cref="CurrentFraction"/> of the current step.</summary>
public sealed record ProgressDto(
    StepKind Step,
    int StepIndex,
    int StepCount,
    double Fraction,
    double? CurrentFraction,
    BroMessage Operation,
    string? Current = null,
    double? SecondsLeft = null)
{
    public static JsonValue Json(ProgressDto progress)
    {
        var m = new List<KeyValuePair<string, JsonValue>>
        {
            new("step", JsonValue.Of(EnumWire.Name(progress.Step))),
            new("stepIndex", JsonValue.Of(progress.StepIndex)),
            new("stepCount", JsonValue.Of(progress.StepCount)),
            new("fraction", JsonDto.Number(progress.Fraction)),
        };
        if (progress.CurrentFraction is { } c) m.Add(new("currentFraction", JsonDto.Number(c)));
        m.Add(new("operation", progress.Operation.ToJson()));
        if (progress.Current is { } current) m.Add(new("current", JsonValue.Of(current)));
        if (progress.SecondsLeft is { } left) m.Add(new("secondsLeft", JsonDto.Number(left)));
        return new JsonValue.Object(m);
    }
}
