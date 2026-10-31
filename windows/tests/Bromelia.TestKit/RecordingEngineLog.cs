using Bromelia.Domain;
using Bromelia.Ports;

namespace Bromelia.TestKit;

/// <summary>An engine log that keeps its lines (the file adapter writes logs/engine.log). Thread-safe.</summary>
public sealed class RecordingEngineLog : IEngineLog
{
    readonly List<BroMessage> _lines = new();

    public void Log(BroMessage message)
    {
        lock (_lines) _lines.Add(message);
    }

    public IReadOnlyList<BroMessage> Lines()
    {
        lock (_lines) return _lines.ToList();
    }
}
