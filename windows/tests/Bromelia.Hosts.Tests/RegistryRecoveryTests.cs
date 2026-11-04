using System.Diagnostics;
using Bromelia.Adapters.Windows;
using Bromeliad;
using Xunit;

namespace Bromelia.Hosts.Tests;

/// <summary>Decision 301 (review L3-7, P2-4): the registry swap a crash left behind is put back by the real root's startup.
/// The adapter's CrashTests and settings-isolation's cases pin RegistrySwapIsolation itself; this pins that the root hands
/// RecoveryService the same isolation and snapshot (&lt;data&gt;/run/makemkv-registry.json) MakemkvTool swaps with. The
/// snapshot is written by RegistrySwapIsolation in Bromelia.TestProbe, killed while it holds the swap (CrashTests' probe),
/// around a BromeliaTest_ value of its own only: never a real MakeMKV setting. A temporary data folder and home, no system
/// keystore or tools, an empty environment.</summary>
public sealed class RegistryRecoveryTests : IDisposable
{
    readonly string _root = Path.Combine(Path.GetTempPath(), "bromelia-host-" + Guid.NewGuid().ToString("N"));

    public void Dispose()
    {
        try { Directory.Delete(_root, true); } catch (IOException) { }
    }

    [Fact]
    public async Task ARegistrySwapACrashLeftIsPutBackWhenTheRealRootStarts()
    {
        var data = Path.Combine(_root, "data");
        var home = Path.Combine(_root, "home");
        Directory.CreateDirectory(home);
        var name = "BromeliaTest_" + Guid.NewGuid().ToString("N");
        var registry = new CurrentUserMakemkvRegistry();
        var snapshot = Path.Combine(data, "run", "makemkv-registry.json");
        try
        {
            Assert.NotEqual(0, Probe("registry", data, name));
            Assert.Equal("run", registry.Get(name));
            Assert.True(File.Exists(snapshot), "the killed probe left its snapshot");
            var engine = CompositionRoot.Build(new RootOptions(data, null, home, SystemKeystore: false, SystemTools: false, Drives: false,
                new Dictionary<string, string>()));
            await engine.Start().WaitAsync(TimeSpan.FromSeconds(30));
            Assert.Equal("user", registry.Get(name));
            Assert.False(File.Exists(snapshot), "the snapshot is removed once every value is back");
            // No registry problem (makemkv.registryNotRestored/registryFailed).
            Assert.DoesNotContain((await engine.System().WaitAsync(TimeSpan.FromSeconds(30))).Problems,
                p => p.Message.Code.ToString().StartsWith("makemkv.registry", StringComparison.Ordinal));
            await engine.Stop().WaitAsync(TimeSpan.FromSeconds(40));
        }
        finally { registry.Remove(name); }
    }

    /// <summary>Bromelia.TestProbe (built next to these tests) run to its end or its own kill; its exit code.</summary>
    static int Probe(params string[] args)
    {
        var host = Environment.GetEnvironmentVariable("DOTNET_HOST_PATH") is { Length: > 0 } h ? h : "dotnet";
        var psi = new ProcessStartInfo(host) { UseShellExecute = false, CreateNoWindow = true };
        psi.ArgumentList.Add(Path.Combine(AppContext.BaseDirectory, "Bromelia.TestProbe.dll"));
        foreach (var a in args) psi.ArgumentList.Add(a);
        using var p = Process.Start(psi)!;
        if (!p.WaitForExit(60_000))
        {
            p.Kill(true);
            throw new TimeoutException("the probe ran for a minute");
        }
        return p.ExitCode;
    }
}
