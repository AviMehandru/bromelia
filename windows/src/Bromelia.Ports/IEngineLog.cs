using Bromelia.Domain;

namespace Bromelia.Ports;

/// <summary>Where the engine writes its own log lines (plan §16), from any thread: recovery.summary at start,
/// internal.unexpected. The host's adapter writes logs/engine.log with each line's time; rotation and the secret
/// redaction come with bromeliad (phase 4).</summary>
public interface IEngineLog
{
    /// <summary>Appends the line (time, severity, code, params); a failure to write is dropped (logging never fails the
    /// engine).</summary>
    void Log(BroMessage message);
}
