using System;
using System.Collections.Generic;
using System.Threading.Tasks;
using Bromelia.Domain;
using Bromelia.Foundation;

namespace Bromelia.Ports;

/// <summary>The job_steps table.</summary>
public interface IStepRepository
{
    /// <summary>Records how step seq ended.</summary>
    Task Save(Id jobId, int seq, StepResult result);

    /// <summary>The steps that succeeded, in order.</summary>
    Task<IReadOnlyList<StepRecord>> Completed(Id jobId);

    /// <summary>Every step, in order.</summary>
    Task<IReadOnlyList<StepRecord>> Load(Id jobId);
}
