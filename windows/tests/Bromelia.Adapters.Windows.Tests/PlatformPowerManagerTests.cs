using System.Diagnostics;
using System.Runtime.Versioning;
using Xunit;

namespace Bromelia.Adapters.Windows.Tests;

/// <summary>platform-adapters.contract.json#power-guard on real Windows.</summary>
[SupportedOSPlatform("windows")]
public sealed class PlatformPowerManagerTests
{
    static bool Eventually(Func<bool> condition)
    {
        var deadline = DateTime.UtcNow.AddSeconds(5);
        while (!condition() && DateTime.UtcNow < deadline) Thread.Sleep(20);
        return condition();
    }

    /// <summary>powercfg /requests (it needs an elevated shell; null when it can't run).</summary>
    static string? Requests()
    {
        try
        {
            var p = Process.Start(new ProcessStartInfo("powercfg", "/requests") { RedirectStandardOutput = true, RedirectStandardError = true, UseShellExecute = false });
            var text = p!.StandardOutput.ReadToEnd();
            p.WaitForExit();
            return p.ExitCode == 0 ? text : null;
        }
        catch (System.ComponentModel.Win32Exception) { return null; }
    }

    [Fact]
    public void EachGuardKeepsTheComputerAwakeUntilTheLastOneGoes()
    {
        if (!OperatingSystem.IsWindows()) return;
        var power = new PlatformPowerManager();
        var first = power.Inhibit("Ripping discs");
        var second = power.Inhibit("Verifying");
        Assert.True(Eventually(() => power.Awake));
        if (Requests() is { } requests) Assert.Contains("dotnet", requests, StringComparison.OrdinalIgnoreCase);
        first.Release();
        first.Release(); // twice changes nothing
        Thread.Sleep(200);
        Assert.True(power.Awake);
        second.Release();
        Assert.True(Eventually(() => !power.Awake));
    }
}
