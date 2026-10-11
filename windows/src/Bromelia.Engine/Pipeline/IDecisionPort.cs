using System.Threading.Tasks;

namespace Bromelia.Engine;

/// <summary>How a step asks the user: the job is awaitingDecision until one of the answers is given.</summary>
public interface IDecisionPort
{
    /// <summary>The id of the answer; job.cancelled when the job is cancelled first.</summary>
    Task<string> Ask(DecisionDto decision);
}
