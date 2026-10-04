namespace Bromelia.Ports;

/// <summary>What moveMerging does when the destination exists: never replace (ConflictNamer picks another
/// name).</summary>
public enum MovePolicy
{
    NeverReplace,
}
