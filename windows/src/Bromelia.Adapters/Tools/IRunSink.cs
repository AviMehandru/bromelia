using Bromelia.Domain;

namespace Bromelia.Adapters;

/// <summary>Where a tool reports what makemkvcon says, event by event (progress, messages, the current title). Called
/// on the tool's thread.</summary>
public interface IRunSink
{
    void Event(RobotEvent @event);
}
