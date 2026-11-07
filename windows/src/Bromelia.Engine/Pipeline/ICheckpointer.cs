using System.Threading.Tasks;
using Bromelia.Foundation;

namespace Bromelia.Engine;

/// <summary>What a resumable step keeps between its attempts in one job (plan §12.2), stored with the step's row
/// (job_steps.checkpoint), implemented by the runner. A checkpoint is JSON with a "kind": RecoveryService resumes a
/// resumeFrom step after a restart only from a checkpoint of the kind the step names (IStep.ResumeCheckpoint).</summary>
public interface ICheckpointer
{
    /// <summary>What an earlier attempt of the step saved in this job (before a restart, or a retry); null when
    /// none.</summary>
    JsonValue? Saved { get; }

    /// <summary>Stores <paramref name="checkpoint"/> with the step's row; done once it is stored. Any thread.</summary>
    Task Save(JsonValue checkpoint);
}
