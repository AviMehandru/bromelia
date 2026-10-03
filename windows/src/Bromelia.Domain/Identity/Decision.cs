namespace Bromelia.Domain;

/// <summary>A decision about a disc and why (plan §3.1). Only the media kind is decided this way so far, so the
/// value is a <see cref="MediaKind"/>; the plan's confidence isn't used by anything yet and is left out.</summary>
public sealed record Decision(MediaKind Value, BroMessage Reason);
