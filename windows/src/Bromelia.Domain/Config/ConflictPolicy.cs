namespace Bromelia.Domain;

/// <summary>When the unit's folder exists: a new folder "Name (2)", merge item by item (never replacing), or fail.</summary>
public enum ConflictPolicy
{
    NewFolder,
    Merge,
    Skip,
}
