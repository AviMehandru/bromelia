namespace Bromelia.Domain;

/// <summary>What hashing one listed file found.</summary>
public enum FileVerdict
{
    Same,
    Changed,
    Missing,
    Unreadable,
}
