using System.Runtime.Versioning;
using System.Text;
using Bromelia.Domain;
using Bromelia.Foundation;
using Bromelia.Ports;
using Xunit;
using CancellationToken = Bromelia.Foundation.CancellationToken;

namespace Bromelia.Adapters.Windows.Tests;

/// <summary>The platform adapters with a real optical drive holding a disc: only when BROMELIA_TEST_DRIVE names the
/// disc's content (video, data, audio; connect the drive or an image to the VM first). The drive list, the content probe,
/// raw reads of \\.\E:, VIDEO_TS for a DVD and index.bdmv for a Blu-ray; with BROMELIA_TEST_DRIVE_EJECT also eject (and
/// close tray) with the monitor watching, at the end.</summary>
[SupportedOSPlatform("windows")]
public sealed class RealDriveTests
{
    static string Expected => Environment.GetEnvironmentVariable("BROMELIA_TEST_DRIVE") is "1" ? "video" : Environment.GetEnvironmentVariable("BROMELIA_TEST_DRIVE") ?? "";

    static bool Enabled => OperatingSystem.IsWindows() && !string.IsNullOrEmpty(Environment.GetEnvironmentVariable("BROMELIA_TEST_DRIVE"));

    static OsDriveState Drive()
    {
        var states = new PlatformDeviceMonitor(new SystemClock()).Snapshot();
        foreach (var s in states) Console.WriteLine($"drive: {s.Drive.Device} | {s.Drive.Identification} | media {s.Media} | mounted at {s.Drive.MountPath}");
        return states.FirstOrDefault(s => s.Media) ?? throw new Xunit.Sdk.XunitException("no drive holds a disc");
    }

    [Fact]
    public void TheDriveAndItsDisc()
    {
        if (!Enabled) return;
        var state = Drive();
        Assert.False(string.IsNullOrEmpty(state.Drive.Identification));
        var control = new PlatformDriveControl(new SystemClock());
        var mount = control.WaitForMount(state.Drive.Device, new Duration(30), new CancellationSource().Token).GetAwaiter().GetResult();
        Assert.Equal(state.Drive.MountPath, mount);
        Assert.Equal(Expected, EnumWire.Name(control.ProbeContent(state.Drive.Device)));

        var reader = control.OpenRaw(state.Drive.Device);
        try
        {
            Assert.True(reader.SectorCount() > 1_000_000);
            var descriptors = reader.Read(16, 3);
            Assert.Equal(3 * 2048, descriptors.Length);
            var ids = Enumerable.Range(0, 3).Select(i => Encoding.ASCII.GetString(descriptors, i * 2048 + 1, 5)).ToList();
            Assert.True(ids.Contains("CD001") || ids.Contains("BEA01"), string.Join(",", ids));
        }
        finally { reader.Close(); }

        if (Expected == "video" && Directory.Exists(Path.Combine(mount!, "VIDEO_TS")))
        {
            using var source = VideoTsByteSource.Open(mount!)!;
            Assert.NotNull(DvdNav.Analyse(source));
            Assert.Equal("DVDVIDEO-VMG", Encoding.ASCII.GetString(source.Read("VIDEO_TS.IFO", 0, 12)));
        }
        else if (Expected == "video") // a Blu-ray
            Assert.Equal("INDX", Encoding.ASCII.GetString(File.ReadAllBytes(Path.Combine(mount!, "BDMV", "index.bdmv")), 0, 4));

        // Last, in the same test: xUnit doesn't order the tests of a class, and the disc is gone afterwards.
        if (!string.IsNullOrEmpty(Environment.GetEnvironmentVariable("BROMELIA_TEST_DRIVE_EJECT"))) EjectAndCloseTray(state, control);
    }

    static void EjectAndCloseTray(OsDriveState state, PlatformDriveControl control)
    {
        var monitor = new PlatformDeviceMonitor(new SystemClock(), new Duration(0.5));
        var seen = new List<DeviceEvent>();
        monitor.Start(e => { lock (seen) seen.Add(e); });
        try
        {
            control.Eject(state.Drive.Device).GetAwaiter().GetResult();
            for (int i = 0; i < 40; i++)
            {
                lock (seen) if (seen.Contains(new DeviceEvent.MediaRemoved(state.Drive.Device))) break;
                Thread.Sleep(250);
            }
            lock (seen) Assert.Contains(new DeviceEvent.MediaRemoved(state.Drive.Device), seen);
            try { control.CloseTray(state.Drive.Device).GetAwaiter().GetResult(); }
            catch (BroFailure f) { Console.WriteLine($"close tray: {f.Error.Code} (a slim drive's tray has no motor)"); }
        }
        finally { monitor.Stop(); }
    }
}
