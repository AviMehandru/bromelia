using Bromelia.Domain;
using Bromelia.Foundation;
using Bromelia.Ports;
using Xunit;
using static Bromelia.Tests.Fixtures;
using CancellationToken = Bromelia.Foundation.CancellationToken;

namespace Bromelia.Adapters.Tests;

/// <summary>shared/fixtures/adapters/menu-ocr.cases.json.</summary>
public sealed class MenuOcrTests : IDisposable
{
    readonly string _base = Path.Combine(Path.GetTempPath(), "bromelia-ocr-" + Guid.NewGuid().ToString("N"));
    /// <summary>A new folder for each case: deleting a folder and making it again under the same name can leave the
    /// Windows SMB client answering for the old one.</summary>
    string _dir;
    int _case;

    public MenuOcrTests() => _dir = Path.Combine(_base, "0");

    public void Dispose()
    {
        try { Directory.Delete(_base, true); } catch (IOException) { }
    }

    string R(string s) => s.Replace("<dir>", _dir).Replace('\\', '/');
    static string Slashes(string s) => s.Replace('\\', '/');

    [Fact]
    public void TheSharedCasesPass()
    {
        RunCases("adapters/menu-ocr.cases.json", (id, given, expect) =>
        {
            _dir = Path.Combine(_base, (++_case).ToString());
            Directory.CreateDirectory(_dir);
            var launcher = new ScriptedProcessLauncher
            {
                Lines = (given["lines"]?.AsArray ?? Array.Empty<JsonValue>()).Select(l => l.AsString!).ToList(),
                ExitCode = (int)(given["exitCode"]?.AsInteger ?? 0),
                WriteArgument = given["writeArgument"] is { } w ? ((int)w["index"]!.AsInteger!.Value, w["text"]!.AsString!) : null,
            };
            var tools = given["located"]!.AsArray!.ToDictionary(t => EnumWire.Parse<ToolKind>(t.AsString)!.Value, t => "/opt/" + t.AsString);
            var ocr = new MenuOcr(launcher, new MapLocator(tools), new DiskFileSystem());
            var cancelSource = new CancellationSource();
            var cancel = cancelSource.Token;
            if (given["cancelled"]?.AsBool == true) cancelSource.Cancel();
            try
            {
                if (given["op"]!.AsString == "extractStills")
                {
                    using var source = VideoTsByteSource.Open(Path_("adapters/video-ts.iso"))!;
                    var cells = given["cells"]!.AsArray!.Select(c => new CellRef(c.AsArray![0].AsString!, c.AsArray![1].AsInteger!.Value, c.AsArray![2].AsInteger!.Value)).ToList();
                    var stills = ocr.ExtractStills(source, cells, _dir, cancel).GetAwaiter().GetResult();
                    Assert.True(expect["error"] == null, "expected " + expect["error"]);
                    Assert.Equal(expect["stills"]!.AsArray!.Select(s => R(s.AsString!)), stills.Select(Slashes));
                }
                else
                {
                    var numbers = ocr.ReadNumbers(given["stills"]!.AsArray!.Select(s => R(s.AsString!)).ToList(), cancel).GetAwaiter().GetResult();
                    Assert.True(expect["error"] == null, "expected " + expect["error"]);
                    Assert.Equal(expect["numbers"]!.AsArray!.Select(n => (int)n.AsInteger!.Value), numbers);
                }
            }
            catch (BroFailure f)
            {
                Assert.Equal(expect["error"]?["code"]?.AsString, f.Error.Code);
                foreach (var m in expect["error"]!["params"]?.AsObject ?? new List<KeyValuePair<string, JsonValue>>())
                    Assert.Equal(m.Value, f.Error.Params[m.Key]);
            }
            if (expect["started"]?.AsInteger is { } started) Assert.Equal(started, launcher.Started.Count);
            if (expect["argv"] is { } argv)
                Assert.Equal(argv.AsArray!.Select(a => string.Join("|", a.AsArray!.Select(x => R(x.AsString!)))),
                    launcher.Started.Select(s => Slashes(string.Join("|", new[] { s.Executable }.Concat(s.Arguments)))));
            if (expect["stallSeconds"]?.AsNumber is { } stall) Assert.All(launcher.Started, s => Assert.Equal(stall, s.StallTimeout?.Seconds));
            if (expect["left"] is { } left)
                Assert.Equal(left.AsArray!.Select(l => l.AsString!), Directory.GetFiles(_dir).Select(Path.GetFileName).OrderBy(n => n, StringComparer.Ordinal));
            return true;
        });
    }
}
