using Bromelia.Core.Engine;
using Bromelia.Daemon;
using Xunit;

namespace Bromelia.Core.Tests;

public class AutomationLockTests
{
    /// <summary>One holder at a time; the next one takes over once the first lets go.</summary>
    [Fact]
    public void OneHolderAtATime()
    {
        var dir = Path.Combine(Path.GetTempPath(), "bromelia-lock-" + Guid.NewGuid().ToString("N")[..6]);
        var path = Path.Combine(dir, "automation.lock");
        try
        {
            var first = new AutomationLock(path);
            using var second = new AutomationLock(path);
            Assert.True(first.Acquire("the app"));
            Assert.True(first.Acquire("the app"));
            Assert.False(second.Acquire("the daemon"));
            Assert.Equal($"the app {Environment.ProcessId}", second.Holder);
            first.Dispose();
            Assert.True(second.Acquire("the daemon"));
            Assert.StartsWith("the daemon ", second.Holder);
        }
        finally { try { Directory.Delete(dir, true); } catch (IOException) { } }
    }
}

public class DaemonTests
{
    [Fact]
    public void Options()
    {
        var (o, error) = Bromelia.Daemon.Options.Parse(new[] { "--config", @"C:\b.json", "-l", "0.0.0.0", "--port", "8080", "--token", "t" });
        Assert.Null(error);
        Assert.Equal(new Bromelia.Daemon.Options(@"C:\b.json", "0.0.0.0", 8080, "t"), o);
        Assert.Equal("Port 0 is not valid", Bromelia.Daemon.Options.Parse(new[] { "--port", "0" }).Error);
        Assert.Equal("Unknown option --bogus", Bromelia.Daemon.Options.Parse(new[] { "--bogus" }).Error);
        Assert.Equal("--config needs a value", Bromelia.Daemon.Options.Parse(new[] { "--config" }).Error);
    }

    [Fact]
    public void LogonTaskArguments()
    {
        var args = LogonTask.CreateArguments(@"C:\Program Files\Bromelia\bromelia-daemon.exe", new[] { "--port", "8080" });
        Assert.Equal(new[] { "/Create", "/F", "/SC", "ONLOGON", "/RL", "LIMITED", "/TN", "Bromelia", "/TR",
                             "\"C:\\Program Files\\Bromelia\\bromelia-daemon.exe\" --port 8080" }, args);
    }
}
