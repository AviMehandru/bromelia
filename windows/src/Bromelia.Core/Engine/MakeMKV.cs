using Bromelia.Core.Config;
using Bromelia.Core.Logic;
using Microsoft.Win32;

namespace Bromelia.Core.Engine;

/// <summary>A source makemkvcon can open.</summary>
public abstract record DiscSource
{
    /// <summary>An optical drive. Index is MakeMKV's drive number, DevicePath the OS device.</summary>
    public sealed record Drive(int Index, string DevicePath) : DiscSource;
    public sealed record Iso(string Path) : DiscSource;
    public sealed record Folder(string Path) : DiscSource;

    /// <summary>Argument for info / mkv. Drives use dev: when the device is known (stable across renumbering).</summary>
    public string InfoArgument => this switch
    {
        Drive d => d.DevicePath.Length == 0 ? $"disc:{d.Index}" : $"dev:{d.DevicePath}",
        Iso i => $"iso:{i.Path}",
        Folder f => $"file:{f.Path}",
        _ => "",
    };

    /// <summary>backup only accepts disc:N.</summary>
    public string? BackupArgument => this is Drive d ? $"disc:{d.Index}" : null;

    public string DisplayName => this switch
    {
        Drive d => d.DevicePath.Length == 0 ? $"Drive {d.Index}" : d.DevicePath,
        Iso i => System.IO.Path.GetFileName(i.Path),
        Folder f => System.IO.Path.GetFileName(f.Path.TrimEnd('/', '\\')),
        _ => "",
    };
}

/// <summary>
/// Everything needed to run makemkvcon with a drive's own configuration.
/// <para>On macOS / Linux the settings go into a private settings.conf under a per-job HOME.</para>
/// <para>On Windows MakeMKV reads HKCU\Software\MakeMKV at start-up, so launches are serialised:
/// the drive's values are written, makemkvcon is started, and the user's values are restored as
/// soon as makemkvcon has produced its first output (i.e. finished reading its settings).</para>
/// </summary>
public sealed class MakeMKVEnvironment
{
    public string Executable { get; }
    public string HomeDirectory { get; }
    public string? ProfilePath { get; }
    public IReadOnlyDictionary<string, string> Settings { get; }
    /// <summary>Keys this environment controls (set or cleared) in the registry.</summary>
    public IReadOnlyCollection<string> ManagedKeys { get; }

    MakeMKVEnvironment(string exe, string home, string? profile, Dictionary<string, string> settings, IReadOnlyCollection<string> managed)
    {
        Executable = exe;
        HomeDirectory = home;
        ProfilePath = profile;
        Settings = settings;
        ManagedKeys = managed;
    }

    public static bool UsesRegistry => OperatingSystem.IsWindows();

    public static MakeMKVEnvironment Prepare(string executable, AppConfig config, DriveConfig drive, string home,
        IEnumerable<string> catalogKeys, string? selectionOverride = null)
    {
        Directory.CreateDirectory(home);
        var settings = config.EffectiveSettings(drive);
        if (!UsesRegistry)
        {
            if (!settings.TryGetValue("app_DataDir", out var dd) || dd.Length == 0) settings["app_DataDir"] = Paths.MakemkvUserFolder;
            else settings["app_DataDir"] = Paths.ExpandUser(dd);
            if (!settings.TryGetValue("app_Key", out var k) || k.Length == 0)
            {
                var installed = InstalledSettings();
                if (installed.TryGetValue("app_Key", out var ik) && ik.Length > 0) settings["app_Key"] = ik;
            }
        }

        string? profilePath = null;
        switch (drive.Profile.Mode)
        {
            case ProfileMode.Generated:
                profilePath = Path.Combine(home, "profile.mmcp.xml");
                File.WriteAllText(profilePath, ProfileBuilder.Build(drive.Profile.Generated, selectionOverride));
                if (selectionOverride != null) settings["app_DefaultSelectionString"] = selectionOverride;
                else if (drive.Profile.Generated.SelectionRule.Length > 0) settings["app_DefaultSelectionString"] = drive.Profile.Generated.SelectionRule;
                break;
            case ProfileMode.CustomFile:
                var p = Paths.ExpandUser(drive.Profile.CustomPath);
                if (p.Length > 0) profilePath = p;
                if (selectionOverride != null) settings["app_DefaultSelectionString"] = selectionOverride;
                break;
            default:
                if (selectionOverride != null) settings["app_DefaultSelectionString"] = selectionOverride;
                break;
        }

        // Keys this environment is responsible for: everything in the catalog plus explicit values.
        var managed = new HashSet<string>(catalogKeys, StringComparer.Ordinal);
        foreach (var key in settings.Keys) managed.Add(key);
        managed.Remove("app_Key");
        if (settings.ContainsKey("app_Key")) managed.Add("app_Key");

        var effective = settings.Where(kv => kv.Value.Length > 0).ToDictionary(kv => kv.Key, kv => kv.Value);
        if (!UsesRegistry)
        {
            var dir = OperatingSystem.IsMacOS() ? Path.Combine(home, "Library", "MakeMKV") : Path.Combine(home, ".MakeMKV");
            Directory.CreateDirectory(dir);
            File.WriteAllText(Path.Combine(dir, "settings.conf"), SettingsConf.Serialize(effective, $"Drive configuration “{drive.Name}”"));
        }
        else
        {
            File.WriteAllText(Path.Combine(home, "settings.conf.txt"),
                SettingsConf.Serialize(effective.Where(kv => kv.Key != "app_Key").ToDictionary(kv => kv.Key, kv => kv.Value),
                    $"Values applied to the registry for “{drive.Name}” (for reference only)"));
        }
        return new MakeMKVEnvironment(executable, home, profilePath, effective, managed);
    }

    /// <summary>The user's MakeMKV settings (registry on Windows, settings.conf elsewhere).</summary>
    public static Dictionary<string, string> InstalledSettings()
    {
        if (UsesRegistry) return RegistrySettings.ReadAll();
        var file = Path.Combine(Paths.MakemkvUserFolder, "settings.conf");
        return File.Exists(file) ? SettingsConf.Parse(File.ReadAllText(file)) : new Dictionary<string, string>();
    }

    // --- arguments ---------------------------------------------------------------------------

    public List<string> CommonSwitches(RipConfig? rip, bool noScan = true)
    {
        var a = new List<string> { "-r", "--progress=-same", "--messages=-stdout" };
        if (noScan) a.Add("--noscan");
        if (ProfilePath != null) a.Add($"--profile={ProfilePath}");
        if (rip != null)
        {
            if (rip.MinLengthSeconds is { } m) a.Add($"--minlength={m}");
            if (rip.CacheMB is { } c && c > 0) a.Add($"--cache={c}");
            if (rip.DirectIO is { } d) a.Add($"--directio={(d ? "true" : "false")}");
            a.AddRange(ArgumentSplitter.Split(rip.ExtraArguments));
        }
        return a;
    }

    public List<string> InfoArguments(DiscSource source, RipConfig? rip)
    {
        var a = CommonSwitches(rip);
        a.Add("info");
        a.Add(source.InfoArgument);
        return a;
    }

    public List<string> MkvArguments(DiscSource source, string title, string destination, RipConfig? rip)
    {
        var a = CommonSwitches(rip);
        a.AddRange(new[] { "mkv", source.InfoArgument, title, destination });
        return a;
    }

    public List<string>? BackupArguments(DiscSource source, bool decrypt, string destination, RipConfig? rip)
    {
        if (source.BackupArgument is not { } s) return null;
        var a = CommonSwitches(rip);
        a.Add("backup");
        if (decrypt) a.Add("--decrypt");
        a.Add(s);
        a.Add(destination);
        return a;
    }

    public static List<string> ScanArguments() => new() { "-r", "--cache=1", "--progress=-same", "--messages=-stdout", "info", "disc:9999" };

    public IDictionary<string, string>? ProcessEnvironment => UsesRegistry ? null : new Dictionary<string, string> { ["HOME"] = HomeDirectory };

    /// <summary>Creates the runner and starts it with this environment's settings in effect.</summary>
    public async Task<ProcessRunner.Result> RunAsync(IEnumerable<string> args, Action<string> onLine, Action<ProcessRunner>? register = null,
        TimeSpan timeout = default, CancellationToken ct = default)
    {
        var runner = new ProcessRunner(Executable, args, ProcessEnvironment, HomeDirectory);
        register?.Invoke(runner);
        if (!UsesRegistry) return await runner.RunAsync(onLine, timeout, ct).ConfigureAwait(false);

        await RegistrySettings.LaunchLock.WaitAsync(ct).ConfigureAwait(false);
        Dictionary<string, string?>? snapshot = null;
        var settingsRead = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var released = 0;
        void Release()
        {
            if (Interlocked.Exchange(ref released, 1) != 0) return;
            if (snapshot != null) RegistrySettings.Restore(snapshot);
            RegistrySettings.LaunchLock.Release();
        }
        try
        {
            snapshot = RegistrySettings.Apply(Settings, ManagedKeys);
            var run = runner.RunAsync(line => { settingsRead.TrySetResult(); onLine(line); }, timeout, ct);
            // Restore as soon as makemkvcon has read its settings (first output), exits, or after 15 s.
            await Task.WhenAny(settingsRead.Task, run, Task.Delay(TimeSpan.FromSeconds(15), CancellationToken.None)).ConfigureAwait(false);
            Release();
            return await run.ConfigureAwait(false);
        }
        finally
        {
            Release();
        }
    }
}

/// <summary>Access to MakeMKV's settings in HKCU\Software\MakeMKV (Windows only).</summary>
public static class RegistrySettings
{
    public const string KeyPath = @"Software\MakeMKV";
    public static readonly SemaphoreSlim LaunchLock = new(1, 1);

    public static Dictionary<string, string> ReadAll()
    {
        var o = new Dictionary<string, string>();
        if (!OperatingSystem.IsWindows()) return o;
        using var key = Registry.CurrentUser.OpenSubKey(KeyPath);
        if (key == null) return o;
        foreach (var name in key.GetValueNames())
            if (key.GetValue(name) is { } v) o[name] = v.ToString() ?? "";
        return o;
    }

    /// <summary>Writes <paramref name="values"/>, clears other managed keys, and returns the previous values.</summary>
    public static Dictionary<string, string?> Apply(IReadOnlyDictionary<string, string> values, IEnumerable<string> managed)
    {
        var snapshot = new Dictionary<string, string?>();
        if (!OperatingSystem.IsWindows()) return snapshot;
        using var key = Registry.CurrentUser.CreateSubKey(KeyPath, writable: true);
        foreach (var name in managed.Concat(values.Keys).Distinct())
        {
            snapshot[name] = key.GetValue(name)?.ToString();
            if (values.TryGetValue(name, out var v)) key.SetValue(name, v, RegistryValueKind.String);
            else if (snapshot[name] != null) key.DeleteValue(name, throwOnMissingValue: false);
        }
        return snapshot;
    }

    public static void Restore(Dictionary<string, string?> snapshot)
    {
        if (!OperatingSystem.IsWindows()) return;
        using var key = Registry.CurrentUser.CreateSubKey(KeyPath, writable: true);
        foreach (var (name, value) in snapshot)
        {
            if (value == null) key.DeleteValue(name, throwOnMissingValue: false);
            else key.SetValue(name, value, RegistryValueKind.String);
        }
    }
}
