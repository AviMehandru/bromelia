using System.Diagnostics;
using System.Runtime.Versioning;
using Bromelia.Domain;
using Bromelia.Foundation;
using Bromelia.Ports;
using Xunit;
using static Bromelia.Tests.Fixtures;
using CancellationToken = Bromelia.Foundation.CancellationToken;

namespace Bromelia.Adapters.Windows.Tests;

/// <summary>shared/fixtures/adapters/drive-control.cases.json, platform-adapters.contract.json#windows-drive-letters, and
/// the VM's empty virtual drive (no eject or tray: real drives are the owner's to test).</summary>
[SupportedOSPlatform("windows")]
public sealed class PlatformDriveControlTests
{
    [Fact]
    public void OnlyABareDriveLetterIsADevice()
    {
        var c = SharedJson("fixtures/adapters/platform-adapters.contract.json")["cases"]!.AsArray!.First(x => x["id"]!.AsString == "windows-drive-letters");
        var paths = c["given"]!["paths"]!.AsArray!;
        var raw = c["given"]!["raw"]!.AsArray!;
        var want = c["expect"]!["devicePaths"]!.AsArray!;
        for (int i = 0; i < paths.Count; i++)
            Assert.Equal(want[i].AsString, PlatformDriveControl.DevicePath(paths[i].AsString!, raw[i].AsBool!.Value));
    }

    [Fact]
    public void TheSharedCasesPass()
    {
        if (!OperatingSystem.IsWindows()) return;
        var drives = new PlatformDriveControl(new SystemClock());
        RunCases("adapters/drive-control.cases.json", (id, given, expect) =>
        {
            if (given["openRaw"]?.AsString is { } open)
            {
                try
                {
                    var reader = drives.OpenRaw(Path_(open));
                    Assert.True(expect["error"] == null, "expected " + expect["error"]);
                    try
                    {
                        Assert.Equal(expect["sectorCount"]!.AsInteger, reader.SectorCount());
                        foreach (var r in expect["reads"]!.AsArray!)
                        {
                            var bytes = reader.Read(r["sector"]!.AsInteger!.Value, (int)r["count"]!.AsInteger!.Value);
                            Assert.Equal(r["length"]!.AsInteger, bytes.Length);
                            if (r["startsHex"]?.AsString is { } hex) Assert.StartsWith(hex, Convert.ToHexString(bytes).ToLowerInvariant());
                        }
                    }
                    finally { reader.Close(); }
                }
                catch (BroFailure f) { Assert.Equal(expect["error"]?.AsString, f.Error.Code); }
            }
            else if (given["probeContent"]?.AsString is { } probe)
                Assert.Equal(expect["content"]!.AsString, EnumWire.Name(drives.ProbeContent(Path_(probe))));
            else
            {
                var watch = Stopwatch.StartNew();
                var mount = drives.WaitForMount(Path_(given["waitForMount"]!.AsString!), new Duration(given["timeout"]!.AsNumber!.Value), new CancellationToken())
                    .GetAwaiter().GetResult();
                Assert.Null(mount);
                Assert.True(watch.Elapsed.TotalSeconds < expect["within"]!.AsNumber!.Value);
            }
            return true;
        });
    }

    /// <summary>Each empty optical drive: unknown content, never mounted within a second.</summary>
    [Fact]
    public void AnEmptyDriveHasNothing()
    {
        if (!OperatingSystem.IsWindows()) return;
        var drives = new PlatformDriveControl(new SystemClock());
        foreach (var d in DriveInfo.GetDrives().Where(d => d.DriveType == DriveType.CDRom && !d.IsReady))
        {
            var device = d.Name.TrimEnd('\\');
            Assert.Equal(DiscContent.Unknown, drives.ProbeContent(device));
            Assert.Null(drives.WaitForMount(device, new Duration(1), new CancellationToken()).GetAwaiter().GetResult());
        }
    }
}
