namespace Bromelia.Engine;

/// <summary>A secret the configuration refers to, and whether it has a value (never the value).</summary>
public sealed record SecretStatus(string Name, bool Set);
