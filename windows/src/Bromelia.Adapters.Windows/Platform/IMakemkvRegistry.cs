namespace Bromelia.Adapters.Windows;

/// <summary>The values of HKCU\Software\MakeMKV, behind an interface so the swap is tested against a fake (plan
/// §26).</summary>
public interface IMakemkvRegistry
{
    /// <summary>The value as text; null when there is none.</summary>
    string? Get(string name);

    /// <summary>Writes a string value.</summary>
    void Set(string name, string value);

    /// <summary>Removes the value (nothing when there is none).</summary>
    void Remove(string name);
}
