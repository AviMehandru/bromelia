using System.Threading.Tasks;
using Bromelia.Domain;

namespace Bromelia.Engine;

/// <summary>One step of a job (plan §12.1). <see cref="Run"/> is called on a worker, never on the executor: it may
/// block, checks its context's cancellation token and reports through the context. An expected failure is thrown as
/// a BroFailure or reported in <see cref="StepReport.Error"/> (with what the outcome needs).</summary>
public interface IStep
{
    StepKind Kind { get; }

    Resumability Resumability { get; }

    /// <summary>The kind of checkpoint (ICheckpointer) a resumeFrom step resumes from by itself after a restart (plan
    /// §12.1's resumeFrom(checkpointKind)); null: none, the step is interrupted.</summary>
    string? ResumeCheckpoint => null;

    /// <summary>What the step holds while it runs: a drive, the acquisition slot, library writes, cpu, io.</summary>
    ResourceSet Resources(JobSnapshot job);

    Task<StepReport> Run(StepContext context);
}
