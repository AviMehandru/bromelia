using System.Diagnostics;
using System.Runtime.Versioning;
using System.Security.AccessControl;
using System.Security.Principal;
using Bromelia.Domain;
using Bromelia.Foundation;
using Bromelia.Ports;
using Xunit;
using CancellationToken = Bromelia.Foundation.CancellationToken;

namespace Bromelia.Adapters.Windows.Tests;

/// <summary>Destinations that can't be written or go away (review follow-ups, failure testing; Linux's
/// /failure/read-only-destination and /share-gone/data-imager, macOS's FailureTests).</summary>
[SupportedOSPlatform("windows")]
public sealed class FailureTests
{
    sealed class NoSink : IRunSink { public void Event(RobotEvent @event) { } }

    sealed class Disc : ISectorReader
    {
        public long Sectors;
        public int DelayMs;
        public byte[] Read(long sector, int count)
        {
            if (DelayMs > 0) Thread.Sleep(DelayMs);
            return new byte[(int)Math.Max(0, Math.Min(count, Sectors - sector)) * 2048];
        }
        public long SectorCount() => Sectors;
        public void Close() { }
    }

    sealed class Drives : IDriveControl
    {
        public Disc? Disc;
        public Task Eject(string device) => Task.CompletedTask;
        public Task CloseTray(string device) => Task.CompletedTask;
        public Task<string?> WaitForMount(string device, Duration timeout, CancellationToken cancel) => Task.FromResult<string?>(null);
        public DiscContent ProbeContent(string device) => DiscContent.Unknown;
        public ISectorReader OpenRaw(string device) => Disc!;
    }

    /// <summary>A folder the user may not write to (a deny rule): the data disc copy and WriteAtomically fail with
    /// fs.failed and leave nothing.</summary>
    [Fact]
    public async Task AReadOnlyDestinationFailsAndLeavesNothing()
    {
        if (!OperatingSystem.IsWindows()) return;
        var info = Directory.CreateDirectory(Path.Combine(Path.GetTempPath(), "bromelia-read-only-" + Guid.NewGuid().ToString("N")));
        var deny = new FileSystemAccessRule(WindowsIdentity.GetCurrent().User!,
            FileSystemRights.CreateFiles | FileSystemRights.CreateDirectories | FileSystemRights.WriteData, AccessControlType.Deny);
        var acl = info.GetAccessControl();
        acl.AddAccessRule(deny);
        info.SetAccessControl(acl);
        try
        {
            var drives = new Drives { Disc = new Disc { Sectors = 512 } };
            var e = await Assert.ThrowsAsync<BroFailure>(() =>
                new DataImager(drives, new PlatformFileSystem()).Copy("D:", Path.Combine(info.FullName, "disc.iso"), new NoSink(), new CancellationSource().Token));
            Assert.Equal("fs.failed", e.Error.Code);
            Assert.Equal("fs.failed", Assert.Throws<BroFailure>(() =>
                new PlatformFileSystem().WriteAtomically(Path.Combine(info.FullName, "r.json"), "{}"u8.ToArray(), 0x1A4)).Error.Code);
            Assert.Empty(Directory.EnumerateFileSystemEntries(info.FullName));
        }
        finally
        {
            acl = info.GetAccessControl();
            acl.RemoveAccessRule(deny);
            info.SetAccessControl(acl);
            info.Delete(true);
        }
    }

    /// <summary>A share that goes away during a copy (opt-in: BROMELIA_TEST_UNMOUNT_DIR, a folder on a share, and
    /// BROMELIA_TEST_UNMOUNT_CMD, a cmd.exe command that takes the share or its server away, run 2 s into the copy):
    /// the copy fails with fs.failed or fs.notFound, never succeeds; how long it took is printed.</summary>
    [Fact]
    public async Task AShareThatGoesAwayFailsTheCopy()
    {
        var dir = Environment.GetEnvironmentVariable("BROMELIA_TEST_UNMOUNT_DIR");
        var command = Environment.GetEnvironmentVariable("BROMELIA_TEST_UNMOUNT_CMD");
        if (string.IsNullOrEmpty(dir) || string.IsNullOrEmpty(command)) return;
        var drives = new Drives { Disc = new Disc { Sectors = 200 * 512, DelayMs = 50 } };
        var later = Task.Run(async () =>
        {
            await Task.Delay(2000);
            Process.Start(new ProcessStartInfo("cmd.exe", "/c " + command) { UseShellExecute = false })!.WaitForExit();
        });
        var watch = Stopwatch.StartNew();
        var e = await Assert.ThrowsAsync<BroFailure>(() =>
            new DataImager(drives, new PlatformFileSystem()).Copy("D:", Path.Combine(dir, "disc.iso"), new NoSink(), new CancellationSource().Token));
        await later;
        Console.WriteLine($"share gone: {e.Error.Code} after {watch.Elapsed.TotalSeconds:F1} s");
        Assert.Contains(e.Error.Code, new[] { "fs.failed", "fs.notFound" });
    }
}
