namespace Bromelia.Domain;

/// <summary>A notification target of the configuration; its URL is the secret named <c>Secret</c> (it carries
/// credentials), resolved by the keystore.</summary>
public sealed record NotifyTarget(string Id, string Secret, string Name = "", bool Enabled = true, bool OnlyProblems = false);
