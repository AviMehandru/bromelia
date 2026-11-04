using Bromelia.Foundation;
using Bromeliad;
using Xunit;

namespace Bromelia.Hosts.Tests;

/// <summary>The real root's smoke test (decision 175): a temporary data folder and home, no system keystore, no system
/// tools (so nothing runs makemkvcon), no drives, an empty environment. Never the default data folder, the system keystore or the
/// user's home.</summary>
public sealed class CompositionRootTests : IDisposable
{
    readonly string _root = Path.Combine(Path.GetTempPath(), "bromelia-host-" + Guid.NewGuid().ToString("N"));

    public void Dispose()
    {
        try { Directory.Delete(_root, true); } catch (IOException) { }
    }

    [Fact]
    public async Task TheRealRootBuildsStartsAnswersAndStops()
    {
        var data = Path.Combine(_root, "data");
        var home = Path.Combine(_root, "home");
        Directory.CreateDirectory(home);
        var engine = CompositionRoot.Build(new RootOptions(data, null, home, SystemKeystore: false, SystemTools: false, Drives: false,
            new Dictionary<string, string>()));
        Assert.True(File.Exists(Path.Combine(data, "bromelia.db")), "the store is opened in the data folder");
        await engine.Start().WaitAsync(TimeSpan.FromSeconds(30));
        var status = await engine.System().WaitAsync(TimeSpan.FromSeconds(30));
        Assert.Equal("1.0.0", status.Facts.Version);
        Assert.Equal(CompositionRoot.AppVersion, status.Facts.Version);
        Assert.Equal("windows", status.Facts.Platform);
        Assert.Equal(data, status.Facts.DataDir);
        Assert.Equal(Environment.ProcessId, status.Facts.Pid);
        Assert.False(status.MakemkvFound);
        Assert.All(status.Tools, t => Assert.Null(t.Path));
        Assert.Empty(status.Libraries);
        await engine.Stop().WaitAsync(TimeSpan.FromSeconds(40));
        var after = await Assert.ThrowsAsync<BroFailure>(() => engine.System());
        Assert.Equal("server.stopping", after.Error.Code);
        Assert.False(Directory.Exists(Path.Combine(data, "secrets")) && Directory.EnumerateFileSystemEntries(Path.Combine(data, "secrets")).Any(),
            "no secret was written");
    }

    /// <summary>I1-5: without drives nothing scans one. With no tools the real scan fails, and start waits for the first
    /// scan, so makemkv.driveScanFailed would be active here.</summary>
    [Fact]
    public async Task NoDriveScanRunsWithoutDrives()
    {
        var data = Path.Combine(_root, "data");
        var home = Path.Combine(_root, "home");
        Directory.CreateDirectory(home);
        var engine = CompositionRoot.Build(new RootOptions(data, null, home, SystemKeystore: false, SystemTools: false, Drives: false,
            new Dictionary<string, string>()));
        await engine.Start().WaitAsync(TimeSpan.FromSeconds(30));
        var status = await engine.System().WaitAsync(TimeSpan.FromSeconds(30));
        Assert.Empty(status.Problems);
        Assert.Empty(await engine.Drives());
        await engine.Stop().WaitAsync(TimeSpan.FromSeconds(40));
    }

    /// <summary>I1-5: with no drives the monitor reports nothing and every drive command is refused.</summary>
    [Fact]
    public async Task NoDrivesReportNothingAndRefuseEverything()
    {
        var monitor = new NoDeviceMonitor();
        var heard = 0;
        monitor.Start(_ => heard++);
        Assert.Empty(monitor.CurrentDrives());
        monitor.Stop();
        Assert.Equal(0, heard);

        var control = new NoDriveControl();
        foreach (var call in new Func<Task>[] { () => control.Eject("/dev/x"), () => control.CloseTray("/dev/x"), () => Task.Run(() => control.OpenRaw("/dev/x")) })
        {
            var failure = await Assert.ThrowsAsync<BroFailure>(call);
            Assert.Equal("drive.notFound", failure.Error.Code);
        }
        Assert.Null(await control.WaitForMount("/dev/x", new Duration(1), new CancellationSource().Token));
        Assert.Equal(Bromelia.Domain.DiscContent.Unknown, control.ProbeContent("/dev/x"));
    }
}
