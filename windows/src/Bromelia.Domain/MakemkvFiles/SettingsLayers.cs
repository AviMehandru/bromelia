using System.Collections.Generic;

namespace Bromelia.Domain;

/// <summary>The settings.conf a job's makemkvcon reads.</summary>
public static class SettingsLayers
{
    /// <summary><paramref name="global"/> (makemkv.settings), then the profile's, then the drive's; empty values are
    /// dropped. app_Key is the registration key when there is one; app_DefaultSelectionString the selection
    /// override (hand-picked tracks: +sel:all) when there is one; app_DataDir, unless a layer sets it, the user's
    /// real MakeMKV data folder (the job runs with its own home).</summary>
    public static Dictionary<string, string> Merge(IReadOnlyDictionary<string, string> global, IReadOnlyDictionary<string, string> profile,
        IReadOnlyDictionary<string, string> drive, string? registrationKey, string? selectionOverride, string? dataDir)
    {
        var o = new Dictionary<string, string>();
        foreach (var layer in new[] { global, profile, drive })
            foreach (var kv in layer)
                o[kv.Key] = kv.Value;
        if (!string.IsNullOrEmpty(registrationKey)) o["app_Key"] = registrationKey;
        if (selectionOverride != null) o["app_DefaultSelectionString"] = selectionOverride;
        if (dataDir is { Length: > 0 } && !(o.TryGetValue("app_DataDir", out var d) && d.Length > 0)) o["app_DataDir"] = dataDir;
        var effective = new Dictionary<string, string>();
        foreach (var kv in o)
            if (kv.Value.Length > 0) effective[kv.Key] = kv.Value;
        return effective;
    }
}
