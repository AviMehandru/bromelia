using Bromelia.Foundation;
using Bromelia.Ports;
using Xunit;
using static Bromelia.Tests.Fixtures;
using CancellationToken = Bromelia.Foundation.CancellationToken;

namespace Bromelia.Adapters.Tests;

/// <summary>shared/fixtures/adapters/mkvtoolnix.cases.json.</summary>
public sealed class MkvToolNixTests : IDisposable
{
    readonly string _root = Path.Combine(Path.GetTempPath(), "bromelia-mkv-" + Guid.NewGuid().ToString("N"));

    public void Dispose()
    {
        try { Directory.Delete(_root, true); } catch (IOException) { }
    }

    string R(string s) => s.Replace("<root>", _root);
    string Shown(string s) => s.Replace(_root, "<root>").Replace('\\', '/');

    [Fact]
    public void TheSharedCasesPass()
    {
        RunCases("adapters/mkvtoolnix.cases.json", (id, given, expect) =>
        {
            if (Directory.Exists(_root)) Directory.Delete(_root, true);
            Directory.CreateDirectory(_root);
            var launcher = new ScriptedProcessLauncher
            {
                Lines = given["lines"]!.AsArray!.Select(l => l.AsString!).ToList(),
                ExitCode = (int)given["exitCode"]!.AsInteger!.Value,
                SplitParts = (int)(given["splitParts"]?.AsInteger ?? 0),
                WriteArgument = given["writeArgument"] is { } w ? ((int)w["index"]!.AsInteger!.Value, w["text"]!.AsString!) : null,
            };
            var tools = new Dictionary<ToolKind, string> { [ToolKind.Mkvmerge] = "/opt/mkvmerge" };
            if (given["mkvextract"] is not { IsNull: true }) tools[ToolKind.Mkvextract] = "/opt/mkvextract";
            var mkv = new MkvToolNix(launcher, new DiskFileSystem(), new MapLocator(tools), _root);
            var call = given["call"]!.AsArray!;
            var cancelSource = new CancellationSource();
            var cancel = cancelSource.Token;
            try
            {
                switch (call[0].AsString)
                {
                    case "probe":
                        var probe = mkv.Probe(R(call[1].AsString!), cancel).GetAwaiter().GetResult();
                        var want = expect["probe"]!;
                        if (want.IsNull) Assert.Null(probe);
                        else
                        {
                            Assert.NotNull(probe);
                            if (want["durationSeconds"]?.AsNumber is { } d) Assert.Equal(d, probe!.DurationSeconds);
                            if (want["tracks"]?.AsInteger is { } t) Assert.Equal(t, probe!.Tracks.Count);
                            if (want["chapterCount"]?.AsInteger is { } c) Assert.Equal(c, probe!.ChapterCount);
                        }
                        break;
                    case "remux":
                        Assert.Equal(expect["ok"]!.AsBool, mkv.Remux(call[1].AsArray!.Select(a => R(a.AsString!)).ToList(), cancel).GetAwaiter().GetResult());
                        break;
                    case "split":
                        var parts = mkv.Split(call[1].AsArray!.Select(a => (int)a.AsInteger!.Value).ToList(), R(call[2].AsString!), cancel).GetAwaiter().GetResult();
                        if (expect["parts"]!.IsNull) Assert.Null(parts);
                        else Assert.Equal(expect["parts"]!.AsInteger, parts!.Count);
                        if (expect["leftOver"]?.AsInteger is { } left)
                            Assert.Equal(left, Directory.GetFiles(_root).Count(f => Path.GetFileName(f).StartsWith(".bromelia-split-")));
                        break;
                    case "chapterTimes":
                        var times = mkv.ChapterTimes(R(call[1].AsString!), cancel).GetAwaiter().GetResult();
                        Assert.Equal(expect["times"]!.AsArray!.Select(t => t.AsNumber!.Value), times!.Select(t => t.Seconds));
                        Assert.Empty(Directory.GetFiles(_root, ".bromelia-chapters-*"));
                        break;
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
            if (expect["executable"]?.AsString is { } exe) Assert.Equal(exe, spec!.Executable);
            if (expect["arguments"] is { } args) Assert.Equal(args.AsArray!.Select(a => a.AsString), spec!.Arguments.Select(Shown));
            if (expect["argumentsStart"] is { } start)
                Assert.Equal(start.AsArray!.Select(a => a.AsString), spec!.Arguments.Take(start.AsArray!.Count).Select(Shown));
            if (expect["argumentsEnd"] is { } end)
                Assert.Equal(end.AsArray!.Select(a => a.AsString), spec!.Arguments.Skip(spec.Arguments.Count - end.AsArray!.Count).Select(Shown));
            return true;
        });
    }
}
