using System.Diagnostics;
using System.Runtime.Versioning;
using Bromelia.Foundation;
using Bromelia.Ports;
using Xunit;

namespace Bromelia.Adapters.Windows.Tests;

/// <summary>The launcher against misbehaving tools (review follow-ups, failure testing; Linux's /abuse/*, macOS's
/// LauncherAbuseTests). Opt-in, since they take a while: BROMELIA_TEST_ABUSE=1.</summary>
[SupportedOSPlatform("windows")]
public sealed class LauncherAbuseTests
{
    static bool Enabled => Environment.GetEnvironmentVariable("BROMELIA_TEST_ABUSE") == "1";

    static ProcessSpec PowerShell(string script) =>
        new("powershell.exe", new[] { "-NoProfile", "-Command", script }, new Dictionary<string, string>(), null, StopPolicy.TerminateFirst);

    /// <summary>300 MB of random bytes, read as they come: every line arrives cut to its limit, memory stays small.</summary>
    [Fact]
    public async Task ABinaryFloodHoldsLittleMemory()
    {
        if (!Enabled) return;
        var script = "$r = [Random]::new(7); $b = New-Object byte[] 1048576; $o = [Console]::OpenStandardOutput(); " +
                     "for ($i = 0; $i -lt 300; $i++) { $r.NextBytes($b); $o.Write($b, 0, $b.Length) }; $o.Flush()";
        var self = Process.GetCurrentProcess();
        self.Refresh();
        var before = self.WorkingSet64 / 1048576.0;
        var p = new PlatformProcessLauncher().Start(PowerShell(script));
        long lines = 0, longest = 0;
        await foreach (var l in p.Lines()) { lines++; longest = Math.Max(longest, l.Text.Length); }
        var exit = await p.Wait();
        self.Refresh();
        var peak = self.PeakWorkingSet64 / 1048576.0;
        Console.WriteLine($"binary flood: {lines} lines, longest {longest} characters, memory {before:F0} MB -> peak {peak:F0} MB");
        Assert.Equal(0, exit.Status);
        Assert.True(longest <= 65536 + 16);
        Assert.True(peak - before < 200, $"{peak - before:F0} MB");
    }

    /// <summary>Five generations of PowerShell, each starting the next: a stop ends every one of them at once (the Job
    /// Object; Windows has no signals to ignore).</summary>
    [Fact]
    public async Task ADeepTreeIsStopped()
    {
        if (!Enabled) return;
        var file = Path.Combine(Path.GetTempPath(), "bromelia-deep-" + Guid.NewGuid().ToString("N") + ".ps1");
        File.WriteAllText(file, "param([int]$n)\n" +
            "if ($n -gt 0) { Start-Process powershell.exe -NoNewWindow -ArgumentList '-NoProfile','-File',$PSCommandPath,($n - 1) }\n" +
            "[Console]::Out.WriteLine($PID); [Console]::Out.Flush(); Start-Sleep 120\n");
        try
        {
            var p = new PlatformProcessLauncher().Start(new ProcessSpec("powershell.exe",
                new[] { "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", file, "4" }, new Dictionary<string, string>(), null, StopPolicy.InterruptFirst));
            var lines = p.Lines().GetAsyncEnumerator();
            var pids = new List<int>();
            while (pids.Count < 5 && await lines.MoveNextAsync()) pids.Add(int.Parse(lines.Current.Text));
            Assert.Equal(5, pids.Count);
            var watch = Stopwatch.StartNew();
            p.Stop(StopReason.Cancelled);
            while (await lines.MoveNextAsync()) { }
            var exit = await p.Wait();
            Console.WriteLine($"deep tree: ended after {watch.Elapsed.TotalSeconds:F1} s");
            Assert.True(exit.Cancelled);
            Assert.True(watch.Elapsed.TotalSeconds < 10);
            await Task.Delay(500);
            foreach (var pid in pids)
                Assert.True(Process.GetProcesses().All(x => x.Id != pid), $"pid {pid} is still running");
        }
        finally { File.Delete(file); }
    }

    /// <summary>65000-character lines while the reader has stopped: the queue holds no more than its byte limit, the
    /// tool waits, and a stop still ends it.</summary>
    [Fact]
    public async Task LongLinesForAReaderThatStoppedHoldLittleMemory()
    {
        if (!Enabled) return;
        var script = "$s = 'x' * 65000; while ($true) { [Console]::Out.WriteLine($s) }";
        var spec = new ProcessSpec("powershell.exe", new[] { "-NoProfile", "-Command", script }, new Dictionary<string, string>(), null,
            StopPolicy.TerminateFirst);
        var self = Process.GetCurrentProcess();
        GC.Collect();
        self.Refresh();
        var before = self.WorkingSet64 / 1048576.0;
        var p = new PlatformProcessLauncher().Start(spec);
        var lines = p.Lines().GetAsyncEnumerator();
        Assert.True(await lines.MoveNextAsync());
        await Task.Delay(TimeSpan.FromSeconds(15)); // not reading
        self.Refresh();
        var held = self.PeakWorkingSet64 / 1048576.0;
        p.Stop(StopReason.Cancelled);
        var drained = 0;
        while (await lines.MoveNextAsync()) drained++;
        var exit = await p.Wait();
        Console.WriteLine($"long lines, reader stopped: memory {before:F0} MB -> peak {held:F0} MB while not reading; {drained} lines drained");
        Assert.True(exit.Cancelled);
        Assert.True(held - before < 150, $"{held - before:F0} MB held");
    }
}
