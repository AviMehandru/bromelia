using System;
using System.Collections.Generic;
using System.Threading.Tasks;
using Bromelia.Domain;
using Bromelia.Foundation;

namespace Bromelia.Ports;

/// <summary>The job_steps table.</summary>
public interface IStepRepository
{
    /// <summary>Writes the step's row (inserted, or replaced for the same job and seq).</summary>
    Task Save(StepRecord step);

    /// <summary>The steps that succeeded, in order.</summary>
    Task<IReadOnlyList<StepRecord>> Completed(Id jobId);

    /// <summary>Every step, in order.</summary>
    Task<IReadOnlyList<StepRecord>> Load(Id jobId);
}
