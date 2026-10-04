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
    public string? Get(string name) => Values.TryGetValue(name, out var v) ? v : null;
    public void Set(string name, string value) { Values[name] = value; Log.Add($"set {name}={value}"); }
    public void Remove(string name) { Values.Remove(name); Log.Add($"remove {name}"); }
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

    [Fact]
    public void TheRunsValuesAreInPlaceUntilMakemkvconHasReadThem()
    {
        if (!OperatingSystem.IsWindows()) return;
        var reg = new FakeRegistry();
        reg.Values["dvd_MinimumTitleLength"] = "120";
        reg.Values["io_ErrorRetryCount"] = "5";
        reg.Values["app_Key"] = "M-purchased";
        reg.Values["unrelated"] = "kept";
        var iso = new RegistrySwapIsolation(new PlatformFileSystem(), reg, Catalog);
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
        if (!OperatingSystem.IsWindows()) return;
        var reg = new FakeRegistry();
        reg.Values["app_Key"] = "T-old";
        var lease = new RegistrySwapIsolation(new PlatformFileSystem(), reg, Catalog).Prepare(Run(new() { ["app_Key"] = "T-new" }));
        Assert.Equal("T-new", reg.Get("app_Key"));
        Assert.DoesNotContain("app_Key", File.ReadAllText(Path.Combine(_root, "home", "settings.conf.txt")));
        lease.Release();
        Assert.Equal("T-old", reg.Get("app_Key"));
    }

    [Fact]
    public async Task LaunchesWaitForTheRegistryAndTheValuesComeBackAfterTheTimeout()
    {
        if (!OperatingSystem.IsWindows()) return;
        var reg = new FakeRegistry();
        reg.Values["dvd_MinimumTitleLength"] = "120";
        var iso = new RegistrySwapIsolation(new PlatformFileSystem(), reg, Catalog, new Duration(0.5));
        var first = iso.Prepare(Run(new() { ["dvd_MinimumTitleLength"] = "1" }));
        var started = DateTime.UtcNow;
        IIsolationLease? second = null;
        var t = Task.Run(() => second = iso.Prepare(Run(new() { ["dvd_MinimumTitleLength"] = "2" })));
        await Task.Delay(200);
        Assert.Null(second);              // waiting for the first launch
        Assert.Same(t, await Task.WhenAny(t, Task.Delay(TimeSpan.FromSeconds(10))));
        Assert.True(DateTime.UtcNow - started >= TimeSpan.FromSeconds(0.4)); // the first gave it back after its timeout
        Assert.Equal("2", reg.Get("dvd_MinimumTitleLength"));
        first.Release();                  // late: changes nothing
        Assert.Equal("2", reg.Get("dvd_MinimumTitleLength"));
        second!.FirstOutput();
        Assert.Equal("120", reg.Get("dvd_MinimumTitleLength"));
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
