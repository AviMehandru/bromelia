using System.Runtime.Versioning;
using Xunit;

namespace Bromelia.Adapters.Windows.Tests;

/// <summary>What a crash leaves behind (review follow-ups, crash points; the same as Linux's /crash/* and macOS's
/// CrashTests): Bromelia.TestProbe kills itself (TerminateProcess) at the chosen point, the test looks at what is left.
/// Phase 3's RecoveryService builds on these.</summary>
[SupportedOSPlatform("windows")]
public sealed class CrashTests : IDisposable
{
    readonly string _dir = Path.Combine(Path.GetTempPath(), "bromelia-crash-" + Guid.NewGuid().ToString("N"));

    public void Dispose()
    {
        try { Directory.Delete(_dir, true); } catch (IOException) { }
    }

    /// <summary>A data disc copy killed after two of its four chunks: the hidden .part file is left, never disc.iso.</summary>
    [Fact]
    public void ADataDiscCopyKilledHalfwayLeavesOnlyThePartFile()
    {
        if (!OperatingSystem.IsWindows()) return;
        Assert.NotEqual(0, TestProbe.Run("image", _dir));
        var left = Directory.EnumerateFileSystemEntries(_dir).Select(Path.GetFileName).ToList();
        Assert.Single(left);
        Assert.StartsWith(".disc.iso.part-", left[0]);
    }

    /// <summary>moveMerging killed around its third report: whatever it reported has moved, and killed before the
    /// report, exactly one more item moved without one.</summary>
    [Theory]
    [InlineData("move-before", 2)]
    [InlineData("move-after", 3)]
    public void MoveMergingKilledReportsOnlyWhatMoved(string mode, int reported)
    {
        if (!OperatingSystem.IsWindows()) return;
        var from = Directory.CreateDirectory(Path.Combine(_dir, "from")).FullName;
        Directory.CreateDirectory(Path.Combine(_dir, "to"));
        for (var i = 0; i < 8; i++) File.WriteAllText(Path.Combine(from, $"f{i}.mkv"), $"f{i}");
        Assert.NotEqual(0, TestProbe.Run(mode, _dir));
        var report = Path.Combine(_dir, "report.txt");
        var lines = File.Exists(report) ? File.ReadAllLines(report).Where(l => l.Length > 0).ToList() : new List<string>();
        foreach (var pair in lines.Select(l => l.Split('\t')))
        {
            Assert.False(File.Exists(pair[0]), pair[0]);
            Assert.True(File.Exists(pair[1]), pair[1]);
        }
        Assert.Equal(reported, lines.Count);
        Assert.Equal(3, Enumerable.Range(0, 8).Count(i => !File.Exists(Path.Combine(from, $"f{i}.mkv"))));
    }

    /// <summary>A registry swap killed while it is held: Recover() puts the user's value back and removes the
    /// snapshot. Only a BromeliaTest_ value of its own is in the catalogue, never a real MakeMKV setting.</summary>
    [Fact]
    public void ARegistrySwapKilledWhileHeldIsPutBack()
    {
        if (!OperatingSystem.IsWindows()) return;
        var name = "BromeliaTest_" + Guid.NewGuid().ToString("N");
        var registry = new CurrentUserMakemkvRegistry();
        var snapshot = Path.Combine(_dir, "run", "makemkv-registry.json");
        try
        {
            Assert.NotEqual(0, TestProbe.Run("registry", _dir, name));
            Assert.Equal("run", registry.Get(name));
            Assert.True(File.Exists(snapshot));
            Assert.True(new RegistrySwapIsolation(new PlatformFileSystem(), registry, new[] { name }, snapshot).Recover());
            Assert.Equal("user", registry.Get(name));
            Assert.False(File.Exists(snapshot));
        }
        finally { registry.Remove(name); }
    }

    static bool Alive(int pid)
    {
        try { using var p = System.Diagnostics.Process.GetProcessById(pid); return !p.HasExited; }
        catch (ArgumentException) { return false; }
    }

    int Pid(string name) => int.Parse(File.ReadAllText(Path.Combine(_dir, name)).Trim());

    /// <summary>The engine killed while a tool runs: the tool and what it started end too (KILL_ON_JOB_CLOSE; on macOS and
    /// Linux the launcher's lifeline), so a crash never leaves makemkvcon reading the disc into a folder marked
    /// INCOMPLETE.</summary>
    [Fact]
    public void AToolDoesNotOutliveAKilledEngine()
    {
        if (!OperatingSystem.IsWindows()) return;
        Directory.CreateDirectory(_dir);
        Assert.NotEqual(0, TestProbe.Run("tool-then-die", _dir));
        int tool = Pid("tool.pid"), child = Pid("child.pid");
        var deadline = DateTime.UtcNow.AddSeconds(10);
        while ((Alive(tool) || Alive(child)) && DateTime.UtcNow < deadline) Thread.Sleep(100);
        var left = (Alive(tool), Alive(child));
        foreach (var pid in new[] { tool, child })
            try { using var p = System.Diagnostics.Process.GetProcessById(pid); p.Kill(); } catch (ArgumentException) { }
        Assert.Equal((false, false), left);
    }

    /// <summary>A tool that exited normally: what it left running is left alone, even after the engine is killed.</summary>
    [Fact]
    public void WhatAFinishedToolLeftRunningIsLeftAlone()
    {
        if (!OperatingSystem.IsWindows()) return;
        Directory.CreateDirectory(_dir);
        Assert.NotEqual(0, TestProbe.Run("tool-exits-then-die", _dir));
        var child = Pid("child.pid");
        Thread.Sleep(1000);
        var alive = Alive(child);
        try { using var p = System.Diagnostics.Process.GetProcessById(child); p.Kill(); } catch (ArgumentException) { }
        Assert.True(alive);
    }
}
