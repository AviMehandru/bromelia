// Bromelia.TestProbe <mode> [args]: started by Bromelia.Adapters.Windows.Tests (PlatformProcessLauncherTests, CrashTests).
//   break-aware               prints "ready"; on CTRL_BREAK prints "break" and exits with 7 (a tool that closes its files)
//   break-ignoring            prints "ready"; on CTRL_BREAK prints "ignored" and keeps running
//   image <folder>            a DataImager copy into <folder>, killed after two of four chunks
//   move-before|move-after <folder>
//                             moveMerging of <folder>\from into <folder>\to (eight files), killed around its third
//                             report, just before or just after it; reports go to <folder>\report.txt
//   registry <folder> <name>  a registry swap of HKCU\Software\MakeMKV\<name> ("user" → "run"), killed while held
//   tool-then-die <folder>    starts "tool <folder> run" with PlatformProcessLauncher, killed while it runs
//   tool-exits-then-die <folder>
//                             starts "tool <folder> exit" and waits for it to end, then is killed
//   tool <folder> run|exit    starts "sleep" (pids in <folder>\tool.pid and child.pid), then sleeps or exits
//   sleep                     sleeps for a minute
// "Killed" is TerminateProcess on itself: no finally, no Dispose, like a crash or a power cut.
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Runtime.Versioning;
using Bromelia.Adapters;
using Bromelia.Adapters.Windows;
using Bromelia.Domain;
using Bromelia.Foundation;
using Bromelia.Ports;
using CancellationToken = Bromelia.Foundation.CancellationToken;

[assembly: SupportedOSPlatform("windows")]

static void Die()
{
    Console.Out.Flush();
    Process.GetCurrentProcess().Kill();
}

switch (args[0])
{
    case "break-aware":
    {
        using var reg = PosixSignalRegistration.Create(PosixSignal.SIGQUIT, ctx => // SIGQUIT is CTRL_BREAK on Windows
        {
            Console.WriteLine("break");
            Console.Out.Flush();
            Environment.Exit(7);
        });
        Console.WriteLine("ready");
        Console.Out.Flush();
        Thread.Sleep(TimeSpan.FromSeconds(60));
        return 1;
    }
    case "break-ignoring":
    {
        using var reg = PosixSignalRegistration.Create(PosixSignal.SIGQUIT, ctx =>
        {
            ctx.Cancel = true;
            Console.WriteLine("ignored");
            Console.Out.Flush();
        });
        Console.WriteLine("ready");
        Console.Out.Flush();
        Thread.Sleep(TimeSpan.FromSeconds(60));
        return 1;
    }
    case "image":
    {
        Directory.CreateDirectory(args[1]);
        var drives = new FakeDriveControl(new FakeDisc(4 * 512, killAfter: 3));
        new DataImager(drives, new PlatformFileSystem()).Copy("D:", Path.Combine(args[1], "disc.iso"), new NoSink(), new CancellationSource().Token)
            .GetAwaiter().GetResult();
        return 1;
    }
    case "move-before":
    case "move-after":
    {
        var from = Path.Combine(args[1], "from");
        var report = Path.Combine(args[1], "report.txt");
        var seen = 0;
        new PlatformFileSystem().MoveMerging(from, Path.Combine(args[1], "to"), MovePolicy.NeverReplace, item =>
        {
            seen++;
            if (seen == 3 && args[0] == "move-before") Die();
            File.AppendAllText(report, item.From + "\t" + item.To + "\n");
            if (seen == 3) Die();
        });
        return 1;
    }
    case "tool-then-die":
    case "tool-exits-then-die":
    {
        var runs = args[0] == "tool-then-die";
        var spec = new ProcessSpec(Environment.ProcessPath!, new[] { typeof(NoSink).Assembly.Location, "tool", args[1], runs ? "run" : "exit" },
            new Dictionary<string, string>(), args[1], StopPolicy.TerminateFirst);
        var tool = new PlatformProcessLauncher().Start(spec);
        if (runs)
            for (var i = 0; i < 200 && !File.Exists(Path.Combine(args[1], "child.pid")); i++) Thread.Sleep(50);
        else
            tool.Wait().GetAwaiter().GetResult();
        Die();
        return 1;
    }
    case "tool":
    {
        var sleeper = Process.Start(new ProcessStartInfo(Environment.ProcessPath!) { UseShellExecute = false, CreateNoWindow = true,
            ArgumentList = { typeof(NoSink).Assembly.Location, "sleep" } })!;
        File.WriteAllText(Path.Combine(args[1], "tool.pid"), Environment.ProcessId.ToString());
        File.WriteAllText(Path.Combine(args[1], "child.pid"), sleeper.Id.ToString());
        if (args[2] == "run") Thread.Sleep(TimeSpan.FromSeconds(60));
        return 0;
    }
    case "sleep":
        Thread.Sleep(TimeSpan.FromSeconds(60));
        return 0;
    case "registry":
    {
        var registry = new CurrentUserMakemkvRegistry();
        var isolation = new RegistrySwapIsolation(new PlatformFileSystem(), registry, new[] { args[2] },
            Path.Combine(args[1], "run", "makemkv-registry.json"));
        registry.Set(args[2], "user");
        isolation.Prepare(new MakemkvRunSettings(new Dictionary<string, string> { [args[2]] = "run" }, null, "", Path.Combine(args[1], "home")));
        Die();
        return 1;
    }
}
return 2;

sealed class NoSink : IRunSink
{
    public void Event(RobotEvent @event) { }
}

sealed class FakeDisc : ISectorReader
{
    readonly long _sectors;
    readonly int _killAfter;
    int _reads;

    public FakeDisc(long sectors, int killAfter)
    {
        _sectors = sectors;
        _killAfter = killAfter;
    }

    public byte[] Read(long sector, int count)
    {
        if (++_reads == _killAfter) Process.GetCurrentProcess().Kill();
        return new byte[(int)Math.Max(0, Math.Min(count, _sectors - sector)) * 2048];
    }

    public long SectorCount() => _sectors;
    public void Close() { }
}

sealed class FakeDriveControl : IDriveControl
{
    readonly FakeDisc _disc;
    public FakeDriveControl(FakeDisc disc) => _disc = disc;
    public Task Eject(string device) => Task.CompletedTask;
    public Task CloseTray(string device) => Task.CompletedTask;
    public Task<string?> WaitForMount(string device, Duration timeout, CancellationToken cancel) => Task.FromResult<string?>(null);
    public DiscContent ProbeContent(string device) => DiscContent.Unknown;
    public ISectorReader OpenRaw(string device) => _disc;
}
