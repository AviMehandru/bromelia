using System.Diagnostics;
using System.Runtime.Versioning;
using System.Text.RegularExpressions;
using Bromelia.Domain;
using Bromelia.Foundation;
using Bromelia.Ports;
using Xunit;

namespace Bromelia.Adapters.Windows.Tests;

/// <summary>shared/fixtures/adapters/platform-adapters.contract.json, the process cases, on real Windows. Each test
/// returns at once elsewhere: this assembly runs on Windows only.</summary>
[SupportedOSPlatform("windows")]
public sealed class PlatformProcessLauncherTests : IDisposable
{
    readonly string _dir = Path.Combine(Path.GetTempPath(), "bromelia-launcher-" + Guid.NewGuid().ToString("N"));
    readonly PlatformProcessLauncher _launcher = new();

    public PlatformProcessLauncherTests() => Directory.CreateDirectory(_dir);

    public void Dispose()
    {
        try { Directory.Delete(_dir, true); } catch (IOException) { } catch (UnauthorizedAccessException) { }
    }

    static ProcessSpec Spec(string exe, params string[] args) =>
        new(exe, args, new Dictionary<string, string>(), null, StopPolicy.InterruptFirst);

    static (List<OutputLine> Lines, ProcessExit Exit) Run(IRunningProcess p)
    {
        var lines = new List<OutputLine>();
        var read = Task.Run(async () => { await foreach (var l in p.Lines()) lines.Add(l); });
        var exit = p.Wait().GetAwaiter().GetResult();
        read.Wait(TimeSpan.FromSeconds(10));
        return (lines, exit);
    }

    string Script(string name, string text)
    {
        var path = Path.Combine(_dir, name);
        File.WriteAllText(path, text.Replace("\n", "\r\n"));
        return path;
    }

    [Fact] // stall-stops
    public void StuckProcessIsStopped()
    {
        if (!OperatingSystem.IsWindows()) return;
        var watch = Stopwatch.StartNew();
        var spec = Spec("cmd.exe", "/c", "echo started& ping -n 60 127.0.0.1 >nul") with { StopPolicy = StopPolicy.TerminateFirst, StallTimeout = new Duration(2) };
        var (lines, exit) = Run(_launcher.Start(spec));
        Assert.NotNull(exit.Stalled);
        Assert.NotEqual(0, exit.Status);
        Assert.False(exit.Cancelled);
        Assert.True(watch.Elapsed < TimeSpan.FromSeconds(15), watch.Elapsed.ToString());
        Assert.Equal(new[] { "started" }, lines.Select(l => l.Text));
    }

    [Fact] // busy-not-stopped
    public void BusyProcessIsNotStopped()
    {
        if (!OperatingSystem.IsWindows()) return;
        var spec = Spec("cmd.exe", "/c", "for /l %i in (1,1,6) do @(echo %i& ping -n 2 127.0.0.1 >nul)") with { StallTimeout = new Duration(2) };
        var (lines, exit) = Run(_launcher.Start(spec));
        Assert.Null(exit.Stalled);
        Assert.Equal(0, exit.Status);
        Assert.Equal(new[] { "1", "2", "3", "4", "5", "6" }, lines.Select(l => l.Text));
    }

    [Theory] // cancel-escalates, terminate-first: Windows has no signals, so the job ends at once either way
    [InlineData(StopPolicy.InterruptFirst)]
    [InlineData(StopPolicy.TerminateFirst)]
    public void StopEndsTheProcess(StopPolicy policy)
    {
        if (!OperatingSystem.IsWindows()) return;
        var watch = Stopwatch.StartNew();
        var p = _launcher.Start(Spec("cmd.exe", "/c", "ping -n 60 127.0.0.1 >nul") with { StopPolicy = policy });
        Thread.Sleep(500);
        p.Stop(StopReason.Cancelled);
        var (_, exit) = Run(p);
        Assert.True(exit.Cancelled);
        Assert.Equal(-1, exit.Status);
        Assert.Null(exit.Signal);
        Assert.True(watch.Elapsed < TimeSpan.FromSeconds(12), watch.Elapsed.ToString());
    }

    [Fact] // output-drained
    public void AChildHoldingThePipeDoesNotHoldUpTheEnd()
    {
        if (!OperatingSystem.IsWindows()) return;
        var script = Script("leaves-child.cmd", "@echo off\nstart /b ping -n 20 127.0.0.1\necho done\n");
        var watch = Stopwatch.StartNew();
        var (lines, exit) = Run(_launcher.Start(Spec(script)));
        Assert.Equal(0, exit.Status);
        Assert.Contains("done", lines.Select(l => l.Text));
        Assert.True(watch.Elapsed < TimeSpan.FromSeconds(9), watch.Elapsed.ToString());
    }

    [Fact] // late-output-goes-nowhere
    public void LateOutputGoesNowhere()
    {
        if (!OperatingSystem.IsWindows()) return;
        var script = Script("late.cmd", "@echo off\nstart /b cmd /c \"ping -n 7 127.0.0.1 >nul & echo late\"\necho early\n");
        var transcript = Path.Combine(_dir, "late.txt");
        var (lines, _) = Run(_launcher.Start(Spec(script) with { Transcript = transcript }));
        var opened = Enumerable.Range(0, 20).Select(i => new FileStream(Path.Combine(_dir, $"opened-{i}"), FileMode.Create, FileAccess.Write, FileShare.Read)).ToList();
        Thread.Sleep(TimeSpan.FromSeconds(3));
        opened.ForEach(f => f.Dispose());
        for (int i = 0; i < 20; i++) Assert.Equal("", File.ReadAllText(Path.Combine(_dir, $"opened-{i}")));
        Assert.Equal(new[] { "early" }, lines.Select(l => l.Text));
        var text = File.ReadAllText(transcript);
        Assert.Contains("\nearly\n", text);
        Assert.DoesNotContain("\nlate\n", text);
    }

    [Fact] // long-line-is-cut
    public void ALongLineIsCut()
    {
        if (!OperatingSystem.IsWindows()) return;
        var script = Script("long.ps1", "[Console]::Out.Write('a' * 200000); [Console]::Out.WriteLine(); 'next'\n");
        var (lines, exit) = Run(_launcher.Start(Spec(script)));
        Assert.Equal(0, exit.Status);
        Assert.Equal(new[] { new string('a', 65536) + " [cut]", "next" }, lines.Select(l => l.Text));
    }

    [Fact] // a-slow-reader-holds-the-tool
    public void ASlowReaderHoldsTheTool()
    {
        if (!OperatingSystem.IsWindows()) return;
        var marker = Path.Combine(_dir, "done.txt");
        var script = Script("many.ps1", "1..50000 | % { [Console]::Out.WriteLine($_) }; Set-Content -Path '" + marker + "' -Value done\n");
        var p = _launcher.Start(Spec(script) with { StallTimeout = new Duration(2) });
        Thread.Sleep(TimeSpan.FromSeconds(3));
        Assert.False(File.Exists(marker)); // waiting to write: at most 10000 lines (and the pipe) wait to be read
        var (lines, exit) = Run(p);
        Assert.Equal(Enumerable.Range(1, 50000).Select(i => i.ToString()), lines.Select(l => l.Text));
        Assert.Null(exit.Stalled);
        Assert.True(File.Exists(marker));
    }

    [Fact] // a-slow-reader-gets-everything
    public async Task ASlowReaderGetsEverything()
    {
        if (!OperatingSystem.IsWindows()) return;
        var script = Script("burst.ps1", "1..30000 | % { [Console]::Out.WriteLine($_) }\n");
        var p = _launcher.Start(Spec(script));
        var lines = new List<string>();
        await foreach (var l in p.Lines())
        {
            lines.Add(l.Text);
            if (lines.Count % 100 == 0) await Task.Delay(25);
        }
        await p.Wait();
        Assert.Equal(Enumerable.Range(1, 30000).Select(i => i.ToString()), lines);
    }

    [Fact] // process-group
    public void StopEndsTheWholeJob()
    {
        if (!OperatingSystem.IsWindows()) return;
        var pidFile = Path.Combine(_dir, "child.pid");
        var script = Script("starts-child.cmd",
            "@echo off\nstart /b powershell -NoProfile -Command \"$PID | Out-File -Encoding ascii '" + pidFile + "'; Start-Sleep 60\"\necho started\nping -n 60 127.0.0.1 >nul\n");
        var p = _launcher.Start(Spec(script));
        string ReadPid()
        {
            try { return File.Exists(pidFile) ? File.ReadAllText(pidFile).Trim() : ""; }
            catch (IOException) { return ""; } // PowerShell still has it open
        }
        var deadline = DateTime.UtcNow.AddSeconds(30);
        while (ReadPid().Length == 0 && DateTime.UtcNow < deadline) Thread.Sleep(100);
        var pid = int.Parse(ReadPid());
        p.Stop(StopReason.Cancelled);
        Run(p);
        Thread.Sleep(500);
        Assert.Throws<ArgumentException>(() => Process.GetProcessById(pid));
    }

    [Fact] // transcript
    public void TheTranscriptHasEveryLineBetweenTheCommandAndTheExit()
    {
        if (!OperatingSystem.IsWindows()) return;
        var transcript = Path.Combine(_dir, "logs", "t.txt");
        Run(_launcher.Start(Spec("cmd.exe", "/c", "echo hi") with { Transcript = transcript }));
        var text = Regex.Replace(File.ReadAllText(transcript), @"\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d{3}Z", "<instant>");
        Assert.Equal("==== <instant> $ cmd.exe /c 'echo hi'\nhi\n==== <instant> exit status 0\n", text);
    }

    [Fact] // transcript-cant-be-written
    public void AMissingTranscriptIsReported()
    {
        if (!OperatingSystem.IsWindows()) return;
        File.WriteAllText(Path.Combine(_dir, "a-file"), "not a folder");
        var transcript = Path.Combine(_dir, "a-file", "t.txt");
        var p = _launcher.Start(Spec("cmd.exe", "/c", "echo hi") with { Transcript = transcript });
        var (lines, exit) = Run(p);
        Assert.Equal(new[] { "hi" }, lines.Select(l => l.Text));
        Assert.Equal(0, exit.Status);
        var problem = p.TranscriptProblem();
        Assert.Equal("process.noTranscript", problem is { } m ? MessageCode.Wire(m.Code) : null);
        Assert.Equal(transcript, problem!.ToJson()["params"]!["path"]!.AsString);
    }

    [Fact]
    public void OutputLinesSayWhichStreamTheyCameFrom()
    {
        if (!OperatingSystem.IsWindows()) return;
        var spec = Spec("cmd.exe", "/c", "echo %BRO_TEST_VALUE%& echo oops 1>&2") with
        {
            Environment = new Dictionary<string, string> { ["BRO_TEST_VALUE"] = "from the spec" },
            WorkingDirectory = _dir,
        };
        var (lines, exit) = Run(_launcher.Start(spec));
        Assert.Equal(0, exit.Status);
        Assert.Contains(lines, l => l.Text == "from the spec" && l.Stream == OutputSource.Stdout);
        Assert.Contains(lines, l => l.Text.TrimEnd() == "oops" && l.Stream == OutputSource.Stderr);
    }

    [Fact]
    public void AProgramThatCannotStartFailsWithCouldNotStart()
    {
        if (!OperatingSystem.IsWindows()) return;
        var e = Assert.Throws<BroFailure>(() => _launcher.Start(Spec(Path.Combine(_dir, "missing.exe"))));
        Assert.Equal("process.couldNotStart", e.Error.Code);
        e = Assert.Throws<BroFailure>(() => _launcher.Start(Spec("cmd.exe", "/c", "echo") with { WorkingDirectory = Path.Combine(_dir, "nope") }));
        Assert.Equal("process.couldNotStart", e.Error.Code);
    }

    [Fact] // interpreter
    public void ScriptsRunThroughTheirInterpreter()
    {
        if (!OperatingSystem.IsWindows()) return;
        string Of(ProcessSpec s) { var c = PlatformProcessLauncher.Invocation(s); return string.Join(" ", new[] { c.Executable }.Concat(c.Arguments)); }
        Assert.Equal("powershell.exe -NoProfile -ExecutionPolicy Bypass -File C:\\s\\a.ps1 x", Of(Spec("C:\\s\\a.ps1", "x")));
        Assert.EndsWith("cmd.exe /c C:\\s\\a.BAT x", Of(Spec("C:\\s\\a.BAT", "x")), StringComparison.OrdinalIgnoreCase);
        Assert.EndsWith("cmd.exe /c C:\\s\\a.cmd", Of(Spec("C:\\s\\a.cmd")), StringComparison.OrdinalIgnoreCase);
        Assert.Equal("py.exe C:\\s\\a.py x", Of(Spec("C:\\s\\a.py", "x")));
        Assert.Equal("C:\\py\\python.exe C:\\s\\a.ps1 x", Of(Spec("C:\\s\\a.ps1", "x") with { Interpreter = "C:\\py\\python.exe" }));
        Assert.Equal("C:\\bin\\tool.exe x", Of(Spec("C:\\bin\\tool.exe", "x")));

        var script = Script("hello.cmd", "@echo off\necho hello %1\n");
        var (lines, exit) = Run(_launcher.Start(Spec(script, "there")));
        Assert.Equal(0, exit.Status);
        Assert.Equal(new[] { "hello there" }, lines.Select(l => l.Text));
    }
}
