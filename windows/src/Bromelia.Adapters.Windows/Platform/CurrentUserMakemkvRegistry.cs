using Microsoft.Win32;

namespace Bromelia.Adapters.Windows;

/// <summary>MakemkvRegistry on HKCU\Software\MakeMKV, string values.</summary>
public sealed class CurrentUserMakemkvRegistry : IMakemkvRegistry
{
    const string KeyPath = @"Software\MakeMKV";

    public string? Get(string name)
    {
        using var key = Registry.CurrentUser.OpenSubKey(KeyPath);
        return key?.GetValue(name)?.ToString();
    }

    public void Set(string name, string value)
    {
        using var key = Registry.CurrentUser.CreateSubKey(KeyPath, writable: true);
        key.SetValue(name, value, RegistryValueKind.String);
    }

    public void Remove(string name)
    {
        using var key = Registry.CurrentUser.OpenSubKey(KeyPath, writable: true);
        key?.DeleteValue(name, throwOnMissingValue: false);
    }
}
