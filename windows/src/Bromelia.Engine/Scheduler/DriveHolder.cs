using Bromelia.Foundation;

namespace Bromelia.Engine;

/// <summary>A drive and the job it was given to.</summary>
public sealed record DriveHolder(string Drive, Id Job);
