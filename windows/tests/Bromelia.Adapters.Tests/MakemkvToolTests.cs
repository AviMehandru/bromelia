using Bromelia.Domain;
using Bromelia.Foundation;
using Bromelia.Ports;
using Bromelia.Tests;
using Xunit;
using static Bromelia.Tests.Fixtures;
using CancellationToken = Bromelia.Foundation.CancellationToken;

namespace Bromelia.Adapters.Tests;

/// <summary>shared/fixtures/adapters/makemkv-tool.cases.json.</summary>
public sealed class MakemkvToolTests : IDisposable
{
    readonly string _root = Path.Combine(Path.GetTempPath(), "bromelia-makemkv-" + Guid.NewGuid().ToString("N"));

    public void Dispose()
    {
        try { Directory.Delete(_root, true); } catch (IOException) { }
    }

    string Shown(string text)
    {
        var root = _root.Replace('\\', '/');
        return text.Replace('\\', '/').Replace(root + "/home", "<work>").Replace(root, "<root>");
    }

    static MakemkvSource Source(JsonValue v) =>
        v["drive"] is { } d ? new MakemkvSource.Drive((int)d["index"]!.AsInteger!.Value, d["device"]!.AsString!)
        : v["iso"] is { } i ? new MakemkvSource.Iso(i.AsString!)
        : new MakemkvSource.File(v["file"]!.AsString!);

    static IReadOnlyList<string> Lines(JsonValue given) =>
        given["transcript"] is { } t ? Text(t.AsString!).Split('\n').Where(l => l.Length > 0).ToList()
        : given["lines"]!.AsArray!.Select(x => x.AsString!).ToList();

    static string Status(StatusWord s) => EnumWire.Name(s);

    [Fact]
    public void TheSharedCasesPass()
    {
        RunCases("adapters/makemkv-tool.cases.json", (id, given, expect) =>
        {
            if (Directory.Exists(_root)) Directory.Delete(_root, true);
            Directory.CreateDirectory(Path.Combine(_root, "home"));
            foreach (var f in given["existing"]?.AsArray ?? new List<JsonValue>())
            {
                var path = Path.Combine(_root, f.AsString!);
                Directory.CreateDirectory(Path.GetDirectoryName(path)!);
                File.WriteAllText(path, "");
            }
            var destination = given["destination"]?.AsString is { } dest ? Path.Combine(_root, dest) : null;
            if (destination != null) Directory.CreateDirectory(given["destinationExists"]?.AsBool == false ? Path.GetDirectoryName(destination)! : destination);
            var writes = (given["writes"]?.AsArray ?? new List<JsonValue>()).Select(w => w.AsString!).ToList();
            var cancel = new CancellationToken();
            var launcher = new ScriptedProcessLauncher
            {
                Lines = Lines(given),
                ExitCode = (int)(given["exitCode"]?.AsInteger ?? 0),
                WriteFileIn = writes.Count > 0 ? destination : null,
                WriteFileNames = writes,
            };
            if (given["cancelAfterLines"]?.AsInteger is { } after) launcher.AfterLine = n => { if (n == after) cancel.Cancel(); };
            var isolation = new RecordingIsolation();
            var makemkvcon = given["makemkvcon"] is { IsNull: true } ? null : "/opt/makemkvcon";
            var tool = new MakemkvTool(launcher, new DiskFileSystem(), isolation, new FixedLocator(makemkvcon));
            var options = new MakemkvOptions(MinLengthSeconds: (int?)given["options"]?["minLengthSeconds"]?.AsInteger);
            var settings = new MakemkvRunSettings(new Dictionary<string, string>(), given["profileXml"]?.AsString, "/data", Path.Combine(_root, "home"));
            var invocation = new MakemkvInvocation(settings, options, given["stallTimeout"]?.AsInteger is { } st ? new Duration(st) : null,
                given["transcriptFile"]?.AsString is { } tf ? Path.Combine(_root, tf) : null);
            var sink = new CountingSink();
            MakemkvRun? run = null;
            IReadOnlyList<MakemkvDrive>? drives = null;
            Listing? listing = null;
            try
            {
                switch (given["call"]!.AsString)
                {
                    case "scanDrives": drives = tool.ScanDrives(cancel).GetAwaiter().GetResult(); break;
                    case "listing":
                        var l = tool.Listing(Source(given["source"]!), invocation, sink, cancel).GetAwaiter().GetResult();
                        listing = l.Listing;
                        run = l.Run;
                        break;
                    case "rip": run = tool.Rip(Source(given["source"]!), given["title"]!.AsString!, destination!, invocation, sink, cancel).GetAwaiter().GetResult(); break;
                    case "backup": run = tool.Backup(Source(given["source"]!), given["decrypt"]!.AsBool!.Value, destination!, invocation, sink, cancel).GetAwaiter().GetResult(); break;
                    default: return false;
                }
                Assert.Null(expect["error"]);
            }
            catch (BroFailure f)
            {
                Assert.Equal(expect["error"]?.AsString, f.Error.Code);
            }

            var spec = launcher.Started.FirstOrDefault();
            if (expect["launched"]?.AsBool == false) Assert.Null(spec);
            if (expect["arguments"] is { } args) Assert.Equal(args.AsArray!.Select(a => a.AsString), spec!.Arguments.Select(Shown));
            if (expect["environment"] is { } env)
                Assert.Equal(env.AsObject!.Select(m => m.Key + "=" + m.Value.AsString), spec!.Environment.OrderBy(kv => kv.Key).Select(kv => kv.Key + "=" + Shown(kv.Value)));
            if (expect["workingDirectory"] is { } wd) Assert.Equal(wd.AsString, spec!.WorkingDirectory is { } w ? Shown(w) : null);
            if (expect["stopPolicy"] is { } sp) Assert.Equal(sp.AsString, EnumWire.Name(spec!.StopPolicy));
            if (expect["stallTimeout"] is { } stall) Assert.Equal(stall.AsInteger, (long?)spec!.StallTimeout?.Seconds);
            if (expect["transcriptFile"] is { } tfile) Assert.Equal(tfile.AsString, Shown(spec!.Transcript!));
            if (expect["lease"] is { } lease) Assert.Equal(lease.AsArray!.Select(x => x.AsString), isolation.Log);
            if (expect["stopped"] is { } stopped) Assert.Equal(stopped.AsBool, launcher.Last!.StoppedBy != null);
            if (expect["stopReason"]?.AsString is { } why) Assert.Equal(why, EnumWire.Name(launcher.Last!.StoppedBy!.Value));
            if (expect["linesRead"] is { } read) Assert.Equal(read.AsInteger, launcher.Last!.Handed);
            if (expect["drives"] is { } want)
                Assert.Equal(want.AsArray!.Select(d => $"{d.AsArray![0].AsInteger} {d.AsArray![1].AsString} {d.AsArray![2].AsString}"),
                    drives!.Select(d => $"{d.Index} {EnumWire.Name(d.State)} {d.Device}"));
            if (expect["listing"] is { } wl)
            {
                Assert.Equal(wl["volumeName"]!.AsString, listing!.VolumeName);
                Assert.Equal(wl["titles"]!.AsInteger, listing.Titles.Count);
            }
            if (expect["status"] is { } status) Assert.Equal(status.AsString, Status(run!.Outcome.Status));
            if (expect["errorContains"]?.AsString is { } contains) Assert.Contains(contains, English.Render(run!.Outcome.Error!.ToJson()));
            if (expect["errorCode"]?.AsString is { } code) Assert.Equal(code, MessageCode.Wire(run!.Outcome.Error!.Code));
            if (expect["version"]?.AsString is { } version) Assert.Equal(version, run!.Version);
            if (expect["debugLog"]?.AsString is { } log) Assert.Equal(log, run!.Outcome.DebugLog);
            if (expect["produced"] is { } produced) Assert.Equal(produced.AsArray!.Select(n => n.AsString), run!.Outcome.Produced);
            if (run != null) Assert.True(sink.Events > 0);
            return true;
        });
    }

    sealed class ThrowingSink : IRunSink
    {
        int _events;
        public void Event(RobotEvent e)
        {
            if (++_events == 3) throw new InvalidOperationException("the sink failed");
        }
    }

    /// <summary>An exception while a line is handled (here the sink's) stops makemkvcon and waits for it before it goes
    /// on: the drive isn't left to a process nobody reads.</summary>
    [Fact]
    public void AnExceptionWhileReadingStopsTheTool()
    {
        Directory.CreateDirectory(Path.Combine(_root, "home"));
        var launcher = new ScriptedProcessLauncher { Lines = Text("mkv-rip.txt").Split('\n').Where(l => l.Length > 0).ToList() };
        var isolation = new RecordingIsolation();
        var tool = new MakemkvTool(launcher, new DiskFileSystem(), isolation, new FixedLocator("/opt/makemkvcon"));
        var invocation = new MakemkvInvocation(new MakemkvRunSettings(new Dictionary<string, string>(), null, "/data", Path.Combine(_root, "home")),
            new MakemkvOptions(), null, null);
        var e = Assert.Throws<InvalidOperationException>(() =>
            tool.Rip(new MakemkvSource.Drive(0, "/dev/rdisk4"), "all", Path.Combine(_root, "staging"), invocation, new ThrowingSink(), new CancellationToken())
                .GetAwaiter().GetResult());
        Assert.Equal("the sink failed", e.Message);
        Assert.Equal(StopReason.Policy, launcher.Last!.StoppedBy);
        Assert.True(launcher.Last.Wait().IsCompleted);
        Assert.Equal(new[] { "prepare", "firstOutput", "release" }, isolation.Log);
    }
}
