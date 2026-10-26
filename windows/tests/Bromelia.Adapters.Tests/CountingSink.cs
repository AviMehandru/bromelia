using Bromelia.Domain;
using Bromelia.Ports;

namespace Bromelia.Adapters.Tests;

internal sealed class CountingSink : IRunSink
{
    public int Events;
    public void Event(RobotEvent @event) => Events++;
}
