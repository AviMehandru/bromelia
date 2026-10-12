namespace Bromelia.Engine;

/// <summary>Where a secret's value can come from, in the order they are asked (config secrets.prefer).</summary>
public enum SecretSource
{
    Keystore,
    Environment,
    File,
}
