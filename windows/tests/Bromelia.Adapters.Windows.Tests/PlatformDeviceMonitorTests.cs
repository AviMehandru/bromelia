using System.Diagnostics;
using System.Runtime.Versioning;
using Bromelia.Domain;
using Bromelia.Foundation;
using Bromelia.Ports;
using Xunit;

namespace Bromelia.Adapters.Windows.Tests;

/// <summary>The drive list on real Windows: the same drives, names and models as WMI's Win32_CDROMDrive.</summary>
[SupportedOSPlatform("windows")]
public sealed class PlatformDeviceMonitorTests
{
    /// <summary>"D: NECVMWar VMware SATA CD01" for each optical drive, from WMI.</summary>
    static List<string> Wmi()
    {
        var p = Process.Start(new ProcessStartInfo("powershell", "-NoProfile -Command \"Get-CimInstance Win32_CDROMDrive | ForEach-Object { $_.Drive + ' ' + $_.Caption }\"")
            { RedirectStandardOutput = true, UseShellExecute = false })!;
        var lines = p.StandardOutput.ReadToEnd().Split('\n').Select(l => l.Trim()).Where(l => l.Length > 0).OrderBy(l => l, StringComparer.Ordinal).ToList();
        p.WaitForExit();
        return lines;
    }

    [Fact]
    public void TheDrivesAreWhatWindowsLists()
    {
        if (!OperatingSystem.IsWindows()) return;
        var monitor = new PlatformDeviceMonitor(new SystemClock());
        var states = monitor.Snapshot();
        // WMI's caption is a display name: the vendor and product, then the bus for some drives ("… USB Device").
        var wmi = Wmi();
        Assert.Equal(wmi.Count, states.Count);
        foreach (var (line, s) in wmi.Zip(states))
            Assert.True(line == s.Drive.Device + " " + s.Drive.Identification || line.StartsWith(s.Drive.Device + " " + s.Drive.Identification + " ", StringComparison.Ordinal),
                $"{line} | {s.Drive.Device} {s.Drive.Identification}");
        foreach (var s in states) Assert.Equal(s.Media, s.Drive.MountPath is not null);
        Assert.Equal(states.Select(s => s.Drive), monitor.CurrentDrives());
    }

    [Fact]
    public void NothingIsReportedWhileNothingChanges()
    {
        if (!OperatingSystem.IsWindows()) return;
        var monitor = new PlatformDeviceMonitor(new SystemClock(), new Duration(0.2));
        var events = new List<DeviceEvent>();
        monitor.Start(e => { lock (events) events.Add(e); });
        Thread.Sleep(1000);
        monitor.Stop();
        lock (events) Assert.Empty(events);
    }
}
