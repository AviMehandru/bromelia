using System.Runtime.Versioning;
using Bromelia.Ports;
using Xunit;

namespace Bromelia.Adapters.Windows.Tests;

[SupportedOSPlatform("windows")]
public sealed class PlatformToolPathsTests
{
    [Fact]
    public void TodaysCandidatesAndNames()
    {
        if (!OperatingSystem.IsWindows()) return;
        var c = PlatformToolPaths.Candidates(@"C:\Users\me");
        Assert.Contains(c[ToolKind.Makemkvcon], p => p.EndsWith(@"\MakeMKV\makemkvcon64.exe", StringComparison.OrdinalIgnoreCase));
        Assert.Contains(c[ToolKind.Mkvmerge], p => p.EndsWith(@"\MKVToolNix\mkvmerge.exe", StringComparison.OrdinalIgnoreCase));
        Assert.Contains(c[ToolKind.Cyanrip], p => p.StartsWith(AppContext.BaseDirectory, StringComparison.OrdinalIgnoreCase));
        var n = PlatformToolPaths.Names();
        Assert.Equal(new[] { "makemkvcon64.exe", "makemkvcon.exe" }, n[ToolKind.Makemkvcon]);
        Assert.Equal(new[] { "HandBrakeCLI.exe" }, n[ToolKind.Handbrake]);
        Assert.Equal(Enum.GetValues<ToolKind>().Length, n.Count);
    }
}
