namespace Bromelia.Engine;

/// <summary>Every event type: the client-visible ones of shared/schema/events.json (wire form: EnumWire.Name), then
/// the internal ones, which are never streamed.</summary>
public enum EngineEventType
{
    Snapshot,
    DriveChanged,
    DriveRemoved,
    MediumArrived,
    MediumRemoved,
    SessionChanged,
    SessionClosed,
    JobChanged,
    JobFinished,
    JobLog,
    QueueChanged,
    ProblemRaised,
    ProblemCleared,
    LibraryChanged,
    MaintenanceProgress,
    ConfigChanged,
    AutomationCountdown,
    ToolsChanged,
    LeaseChanged,
    StepFinished,
}

/// <summary>C# enums have no methods.</summary>
public static class EngineEventTypes
{
    /// <summary>leaseChanged and stepFinished: in-engine only, never streamed.</summary>
    public static bool IsInternal(this EngineEventType type) =>
        type is EngineEventType.LeaseChanged or EngineEventType.StepFinished;
}
