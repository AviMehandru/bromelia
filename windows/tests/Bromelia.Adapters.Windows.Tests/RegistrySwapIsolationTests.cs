using System.Runtime.Versioning;
using Bromelia.Foundation;
using Bromelia.Ports;
using Xunit;

namespace Bromelia.Adapters.Windows.Tests;

/// <summary>The registry, in memory.</summary>
internal sealed class FakeRegistry : IMakemkvRegistry
{
    public readonly Dictionary<string, string> Values = new();
    public readonly List<string> Log = new();
    /// <summary>Writes fail as a locked-down key would.</summary>
    public bool Fails;
    public string? Get(string name) => Values.TryGetValue(name, out var v) ? v : null;
    public void Set(string name, string value) { Refuse(); Values[name] = value; Log.Add($"set {name}={value}"); }
    public void Remove(string name) { Refuse(); Values.Remove(name); Log.Add($"remove {name}"); }
    void Refuse() { if (Fails) throw new UnauthorizedAccessException("Access is denied."); }
}

/// <summary>platform-adapters.contract.json#settings-isolation-registry, against a fake registry; and the real
/// HKCU\Software\MakeMKV with a value of its own.</summary>
[SupportedOSPlatform("windows")]
public sealed class RegistrySwapIsolationTests : IDisposable
{
    readonly string _root = Path.Combine(Path.GetTempPath(), "bromelia-registry-" + Guid.NewGuid().ToString("N"));

    public void Dispose()
    {
        try { Directory.Delete(_root, true); } catch (IOException) { }
    }

    static readonly string[] Catalog = { "app_DataDir", "app_DefaultSelectionString", "dvd_MinimumTitleLength", "io_ErrorRetryCount" };

    MakemkvRunSettings Run(Dictionary<string, string> settings, string? profile = null) => new(settings, profile, "", Path.Combine(_root, "home"));

    string Snapshot => Path.Combine(_root, "run", "makemkv-registry.json");

    /// <summary>The temporary folder is on a network share (the SMB runs): no snapshot can be written, so the tests
    /// that need one step aside and ASnapshotOnANetworkShareIsRefused runs instead.</summary>
    bool OnShare => OwnerOnly.IsOnNetworkShare(_root);

    RegistrySwapIsolation Isolation(FakeRegistry reg, Duration? restoreAfter = null) =>
        new(new PlatformFileSystem(), reg, Catalog, Snapshot, restoreAfter);

    [Fact]
    public void TheRunsValuesAreInPlaceUntilMakemkvconHasReadThem()
    {
        if (!OperatingSystem.IsWindows() || OnShare) return;
        var reg = new FakeRegistry();
        reg.Values["dvd_MinimumTitleLength"] = "120";
        reg.Values["io_ErrorRetryCount"] = "5";
        reg.Values["app_Key"] = "M-purchased";
        reg.Values["unrelated"] = "kept";
        var iso = Isolation(reg);
        var lease = iso.Prepare(Run(new() { ["dvd_MinimumTitleLength"] = "30" }, "<profile/>"));

        // The run's value, the other catalogue keys cleared, app_Key and keys outside the catalogue untouched.
        Assert.Equal("30", reg.Get("dvd_MinimumTitleLength"));
        Assert.Null(reg.Get("io_ErrorRetryCount"));
        Assert.Equal("M-purchased", reg.Get("app_Key"));
        Assert.Equal("kept", reg.Get("unrelated"));
        Assert.Empty(lease.Environment());
        Assert.Equal(Path.Combine(_root, "home") + "\\profile.mmcp.xml", lease.ProfilePath());
        Assert.Equal("<profile/>", File.ReadAllText(lease.ProfilePath()!));
        Assert.Contains("dvd_MinimumTitleLength = \"30\"", File.ReadAllText(Path.Combine(_root, "home", "settings.conf.txt")));

        lease.FirstOutput();
        Assert.Equal("120", reg.Get("dvd_MinimumTitleLength"));
        Assert.Equal("5", reg.Get("io_ErrorRetryCount"));
        Assert.Equal("M-purchased", reg.Get("app_Key"));
        var writes = reg.Log.Count;
        lease.Release(); // once only
        Assert.Equal(writes, reg.Log.Count);
    }

    [Fact]
    public void AppKeyIsReplacedOnlyWhenTheRunSetsIt()
    {
        if (!OperatingSystem.IsWindows() || OnShare) return;
        var reg = new FakeRegistry();
        reg.Values["app_Key"] = "T-old";
        var lease = Isolation(reg).Prepare(Run(new() { ["app_Key"] = "T-new" }));
        Assert.Equal("T-new", reg.Get("app_Key"));
        Assert.DoesNotContain("app_Key", File.ReadAllText(Path.Combine(_root, "home", "settings.conf.txt")));
        lease.Release();
        Assert.Equal("T-old", reg.Get("app_Key"));
    }

    [Fact]
    public async Task LaunchesWaitForTheRegistryAndTheValuesComeBackAfterTheTimeout()
    {
        if (!OperatingSystem.IsWindows() || OnShare) return;
        var reg = new FakeRegistry();
        reg.Values["dvd_MinimumTitleLength"] = "120";
        var iso = Isolation(reg, new Duration(0.5));
        var started = DateTime.UtcNow;
        var first = iso.Prepare(Run(new() { ["dvd_MinimumTitleLength"] = "1" }));
        IIsolationLease? second = null;
        var secondAt = DateTime.MinValue;
        var t = Task.Run(() =>
        {
            second = iso.Prepare(Run(new() { ["dvd_MinimumTitleLength"] = "2" }));
            secondAt = DateTime.UtcNow;
        });
        Assert.Same(t, await Task.WhenAny(t, Task.Delay(TimeSpan.FromSeconds(10))));
        // The second waited for the first launch, which gave the registry back after its timeout (only a lower bound:
        // a busy machine may run either thread late).
        Assert.True(secondAt - started >= TimeSpan.FromSeconds(0.45), (secondAt - started).ToString());
        Assert.Equal("2", reg.Get("dvd_MinimumTitleLength"));
        first.Release();                  // late: changes nothing
        Assert.Equal("2", reg.Get("dvd_MinimumTitleLength"));
        second!.FirstOutput();
        Assert.Equal("120", reg.Get("dvd_MinimumTitleLength"));
    }

    [Fact]
    public void TheUsersValuesAreOnDiskWhileARunsAreInPlace()
    {
        if (!OperatingSystem.IsWindows() || OnShare) return;
        var reg = new FakeRegistry();
        reg.Values["dvd_MinimumTitleLength"] = "120";
        reg.Values["app_Key"] = "M-purchased";
        var lease = Isolation(reg).Prepare(Run(new() { ["dvd_MinimumTitleLength"] = "30", ["app_Key"] = "T-beta" }));
        var saved = JsonValue.Parse(File.ReadAllBytes(Snapshot))!["values"]!;
        Assert.Equal("120", saved["dvd_MinimumTitleLength"]!.AsString);
        Assert.Equal("M-purchased", saved["app_Key"]!.AsString);
        Assert.True(saved["io_ErrorRetryCount"]!.IsNull);
        Assert.True(OwnerOnly.Holds(Path.GetDirectoryName(Snapshot)!));
        Assert.True(OwnerOnly.Holds(Snapshot));
        lease.FirstOutput();
        Assert.False(File.Exists(Snapshot));
        Assert.Equal("M-purchased", reg.Get("app_Key"));
    }

    /// <summary>A crash with a run's values in place: the snapshot left behind puts the user's back, at startup
    /// (Recover) or before the next run.</summary>
    [Fact]
    public void ASnapshotLeftByACrashIsPutBack()
    {
        if (!OperatingSystem.IsWindows() || OnShare) return;
        var reg = new FakeRegistry();
        reg.Values["dvd_MinimumTitleLength"] = "120";
        reg.Values["app_Key"] = "M-purchased";
        var lease = Isolation(reg).Prepare(Run(new() { ["dvd_MinimumTitleLength"] = "30", ["app_Key"] = "T-beta" }));
        var left = File.ReadAllBytes(Snapshot);
        lease.Release();

        // As the crash left it: the run's values in the registry, the snapshot on disk.
        reg.Values["dvd_MinimumTitleLength"] = "30";
        reg.Values["app_Key"] = "T-beta";
        File.WriteAllBytes(Snapshot, left);
        Assert.True(Isolation(reg).Recover());
        Assert.Equal("120", reg.Get("dvd_MinimumTitleLength"));
        Assert.Equal("M-purchased", reg.Get("app_Key"));
        Assert.False(File.Exists(Snapshot));
        Assert.False(Isolation(reg).Recover());

        // Again, found by the next run instead: its snapshot holds the user's values, not the leftover ones.
        reg.Values["dvd_MinimumTitleLength"] = "30";
        reg.Values["app_Key"] = "T-beta";
        File.WriteAllBytes(Snapshot, left);
        var next = Isolation(reg).Prepare(Run(new() { ["dvd_MinimumTitleLength"] = "45" }));
        Assert.Equal("45", reg.Get("dvd_MinimumTitleLength"));
        Assert.Equal("120", JsonValue.Parse(File.ReadAllBytes(Snapshot))!["values"]!["dvd_MinimumTitleLength"]!.AsString);
        next.Release();
        Assert.Equal("120", reg.Get("dvd_MinimumTitleLength"));
        Assert.Equal("M-purchased", reg.Get("app_Key"));
    }

    /// <summary>Values that can't be put back stay on disk, and no run starts until they are back.</summary>
    [Fact]
    public void AFailedRestoreKeepsTheSnapshotAndStopsTheNextRun()
    {
        if (!OperatingSystem.IsWindows() || OnShare) return;
        var reg = new FakeRegistry();
        reg.Values["dvd_MinimumTitleLength"] = "120";
        var lease = Isolation(reg).Prepare(Run(new() { ["dvd_MinimumTitleLength"] = "30" }));
        reg.Fails = true;
        lease.Release();
        Assert.True(File.Exists(Snapshot));
        var failure = Assert.Throws<BroFailure>(() => Isolation(reg).Prepare(Run(new() { ["dvd_MinimumTitleLength"] = "45" })));
        Assert.Equal("makemkv.registryNotRestored", failure.Error.Code);
        Assert.Equal("30", reg.Get("dvd_MinimumTitleLength"));
        reg.Fails = false;
        Isolation(reg).Prepare(Run(new() { ["dvd_MinimumTitleLength"] = "45" })).Release();
        Assert.Equal("120", reg.Get("dvd_MinimumTitleLength"));
        Assert.False(File.Exists(Snapshot));
    }

    /// <summary>The snapshot can hold app_Key: on a network share it is refused before any value changes, and the next
    /// launch isn't left waiting.</summary>
    [Fact]
    public void ASnapshotOnANetworkShareIsRefused()
    {
        if (!OperatingSystem.IsWindows() || !OnShare) return;
        var reg = new FakeRegistry();
        reg.Values["dvd_MinimumTitleLength"] = "120";
        reg.Values["app_Key"] = "M-purchased";
        for (var i = 0; i < 2; i++)
        {
            var failure = Assert.Throws<BroFailure>(() => Isolation(reg).Prepare(Run(new() { ["dvd_MinimumTitleLength"] = "30", ["app_Key"] = "T-beta" })));
            Assert.Equal("fs.notPrivate", failure.Error.Code);
        }
        Assert.Empty(reg.Log);
        Assert.False(File.Exists(Snapshot));
    }

    [Fact]
    public void TheRealRegistryRoundTrips()
    {
        if (!OperatingSystem.IsWindows()) return;
        var reg = new CurrentUserMakemkvRegistry();
        var name = "BromeliaTest_" + Guid.NewGuid().ToString("N");
        Assert.Null(reg.Get(name));
        reg.Set(name, "value");
        Assert.Equal("value", reg.Get(name));
        reg.Remove(name);
        Assert.Null(reg.Get(name));
        reg.Remove(name);
    }
}
