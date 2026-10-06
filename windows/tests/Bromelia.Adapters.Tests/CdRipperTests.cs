using Bromelia.Domain;
using Bromelia.Foundation;
using Bromelia.Ports;
using Xunit;
using static Bromelia.Tests.Fixtures;
using CancellationToken = Bromelia.Foundation.CancellationToken;

namespace Bromelia.Adapters.Tests;

/// <summary>shared/fixtures/adapters/cd-ripper.cases.json.</summary>
public sealed class CdRipperTests : IDisposable
{
    readonly string _base = Path.Combine(Path.GetTempPath(), "bromelia-cd-" + Guid.NewGuid().ToString("N"));
    /// <summary>A new folder for each case: deleting a folder and making it again under the same name can leave the
    /// Windows SMB client answering for the old one.</summary>
    string _root;
    int _case;

    public CdRipperTests() => _root = Path.Combine(_base, "0");

    public void Dispose()
    {
        try { Directory.Delete(_base, true); } catch (IOException) { }
    }

    string R(string s) => s.Replace("<root>", _root);
    static string Slashes(string? s) => (s ?? "").Replace('\\', '/');

    [Fact]
    public void TheSharedCasesPass()
    {
        RunCases("adapters/cd-ripper.cases.json", (id, given, expectAll) =>
        {
            var expect = OperatingSystem.IsWindows() && Json("adapters/cd-ripper.cases.json")["cases"]!.AsArray!
                .First(c => c["id"]!.AsString == id)["windows"] is { } w ? w : expectAll;
            _root = Path.Combine(_base, (++_case).ToString());
            var dest = R(given["dest"]!.AsString!);
            Directory.CreateDirectory(dest);
            var lines = new List<string>();
            var stderr = new HashSet<int>();
            foreach (var l in given["lines"]?.AsArray ?? Array.Empty<JsonValue>())
            {
                if (l["stderr"] is not null) stderr.Add(lines.Count);
                lines.Add(l.AsString ?? l["stderr"]!.AsString!);
            }
            var launcher = new ScriptedProcessLauncher
            {
                Lines = lines, Stderr = stderr, ExitCode = (int)(given["exitCode"]?.AsInteger ?? 0), Stalls = given["stalls"]?.AsBool == true,
            };
            if (given["saves"]?.AsString is { } saves)
            {
                launcher.WriteFileIn = dest;
                launcher.WriteFileNames = new[] { saves };
            }
            var tools = given["located"]!.AsArray!.ToDictionary(t => EnumWire.Parse<ToolKind>(t.AsString)!.Value, t => "/opt/" + t.AsString);
            var ripper = new CdRipper(launcher, new MapLocator(tools), new DiskFileSystem());
            var cancelSource = new CancellationSource();
            var cancel = cancelSource.Token;
            if (given["cancelled"]?.AsBool == true) cancelSource.Cancel();
            var sink = new RecordingSink();
            try
            {
                var files = ripper.Rip(given["device"]!.AsString!, dest, given["command"]?.AsString, (int)given["stallMinutes"]!.AsInteger!.Value, sink, cancel)
                    .GetAwaiter().GetResult();
                Assert.True(expect["error"] == null, "expected " + expect["error"]);
                Assert.Equal(expect["files"]!.AsArray!.Select(f => Slashes(R(f.AsString!))), files.Select(Slashes));
            }
            catch (BroFailure f)
            {
                Assert.Equal(expect["error"]?["code"]?.AsString, f.Error.Code);
                foreach (var m in expect["error"]!["params"]?.AsObject ?? new List<KeyValuePair<string, JsonValue>>())
                    Assert.Equal(m.Value, f.Error.Params[m.Key]);
            }
            if (expect["started"]?.AsBool == false) Assert.Empty(launcher.Started);
            if (expect["argv"] is { } argv)
                Assert.Equal(argv.AsArray!.Select(a => a.AsString!), new[] { launcher.Started[0].Executable }.Concat(launcher.Started[0].Arguments));
            if (expect["workingDirectory"]?.AsString is { } wd) Assert.Equal(Slashes(R(wd)), Slashes(launcher.Started[0].WorkingDirectory));
            if (expect["stallSeconds"] is { } stall) Assert.Equal(stall.AsNumber, launcher.Started[0].StallTimeout?.Seconds);
            if (expect["raw"] is { } raw)
                Assert.Equal(raw.AsArray!.Select(r => r.AsString!), sink.Events.OfType<RobotEvent.Raw>().Select(r => r.Text));
            return true;
        });
    }
}
