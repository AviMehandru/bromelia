namespace Bromelia.Domain;

/// <summary>What a job notification says: the mode, what was ripped (the identified name, else the disc label),
/// how many files went where, and the job's error (null: none).</summary>
public sealed record JobSummary(RipMode Mode, string What, int Files, string Path, BroMessage? Error = null);
