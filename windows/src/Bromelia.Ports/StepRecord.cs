using System;
using System.Collections.Generic;
using System.Threading.Tasks;
using Bromelia.Domain;
using Bromelia.Foundation;

namespace Bromelia.Ports;

/// <summary>A row of job_steps: the durable checkpoint between steps.</summary>
public sealed record StepRecord(
    Id JobId,
    int Seq,
    StepKind Kind,
    StepState State,
    int Attempt,
    Instant? StartedAt = null,
    Instant? FinishedAt = null,
    StepOutput? Output = null,
    BroError? Error = null,
    JsonValue? Checkpoint = null);
