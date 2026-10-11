using Bromelia.Domain;

namespace Bromelia.Engine;

/// <summary>One answer of a decision: its id and its label.</summary>
public sealed record DecisionAnswer(string Id, BroMessage Label);
