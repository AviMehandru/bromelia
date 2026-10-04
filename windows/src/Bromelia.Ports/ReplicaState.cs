namespace Bromelia.Ports;

/// <summary>replicas.state.</summary>
public enum ReplicaState
{
    Pending,
    Copying,
    Verified,
    Stale,
    Failed,
}
