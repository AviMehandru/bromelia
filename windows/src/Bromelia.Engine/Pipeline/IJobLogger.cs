using Bromelia.Domain;

namespace Bromelia.Engine;

/// <summary>How a step logs, from its worker: a jobLog event at once; jobs/&lt;id&gt;/log.jsonl soon after, and when
/// the step ends.</summary>
public interface IJobLogger
{
    void Log(BroMessage message);
}
