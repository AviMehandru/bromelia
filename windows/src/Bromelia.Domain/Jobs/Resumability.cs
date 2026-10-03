namespace Bromelia.Domain;

/// <summary>What a step can do after the engine stopped while it ran (plan §12.1, §20.4).</summary>
public enum Resumability
{
    Idempotent,
    ResumeFrom,
    NotResumable,
}
