using Bromelia.Foundation;

namespace Bromelia.Domain;

/// <summary>A job's outcome and the error that explains it (null for a plain success).</summary>
public sealed record Decided(Outcome Outcome, BroError? Error);
