namespace Bromelia.Domain;

/// <summary>A layer of ProfileResolver.resolve, from lowest to highest.</summary>
public enum ResolveLayer
{
    Defaults,
    DefaultProfile,
    DriveProfile,
    DriveOverrides,
    Rule,
    Session,
}
