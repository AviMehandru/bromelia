namespace Bromelia.Domain;

/// <summary>Why one title is in or out of a selection.</summary>
public sealed record SelectionTrace(int Index, bool Selected, BroMessage Reason);
