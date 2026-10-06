using Bromelia.Foundation;
using Bromelia.Ports;
using Xunit;
using static Bromelia.Tests.Fixtures;

namespace Bromelia.Adapters.Tests;

/// <summary>shared/fixtures/adapters/tool-locator.cases.json.</summary>
public sealed class SystemToolLocatorTests : IDisposable
{
    readonly string _base = Path.Combine(Path.GetTempPath(), "bromelia-tools-" + Guid.NewGuid().ToString("N"));
    /// <summary>A new folder for each case: deleting a folder and making it again under the same name can leave the
    /// Windows SMB client answering for the old one.</summary>
    string _root = "";
    int _case;

    public void Dispose()
    {
        try { Directory.Delete(_base, true); } catch (IOException) { }
    }

    string R(string text) => text.Replace("<root>", _root);

    string Shown(string path) => "<root>/" + Path.GetRelativePath(_root, path).Replace('\\', '/');

    static Dictionary<ToolKind, IReadOnlyList<string>> Lists(JsonValue? v, Func<string, string> map) =>
        (v?.AsObject ?? new List<KeyValuePair<string, JsonValue>>())
            .ToDictionary(m => EnumWire.Parse<ToolKind>(m.Key)!.Value, m => (IReadOnlyList<string>)m.Value.AsArray!.Select(x => map(x.AsString!)).ToList());

    [Fact]
    public void TheSharedCasesPass()
    {
        RunCases("adapters/tool-locator.cases.json", (id, given, expect) =>
        {
            _root = Path.Combine(_base, (++_case).ToString());
            Directory.CreateDirectory(_root);
            foreach (var f in given["files"]!.AsArray!.Select(x => x.AsString!))
            {
                var path = Path.Combine(_root, f.TrimEnd('/'));
                if (f.EndsWith('/')) Directory.CreateDirectory(path);
                else
                {
                    Directory.CreateDirectory(Path.GetDirectoryName(path)!);
                    File.WriteAllText(path, "");
                }
            }
            var configured = given["configured"]!.AsObject!.ToDictionary(m => EnumWire.Parse<ToolKind>(m.Key)!.Value, m => R(m.Value.AsString!));
            var locator = new SystemToolLocator(new DiskFileSystem(), configured, Lists(given["candidates"], R), Lists(given["names"], x => x),
                given["searchPath"]!.AsArray!.Select(x => R(x.AsString!)).ToList(), R(given["home"]!.AsString!));
            var info = locator.Locate(EnumWire.Parse<ToolKind>(given["tool"]!.AsString)!.Value);
            if (expect["path"]!.AsString is { } want)
            {
                Assert.Equal(want, info.Path is null ? "(none)" : Shown(info.Path));
                Assert.Null(info.Why);
            }
            else
            {
                Assert.Null(info.Path);
                Assert.Equal(expect["why"]!["code"]!.AsString, MessageCodeWire(info.Why!));
                foreach (var m in expect["why"]!["params"]!.AsObject!)
                {
                    var actual = info.Why!.Params[m.Key]!.AsString!;
                    Assert.Equal(m.Value.AsString, m.Key == "path" ? Shown(actual) : actual);
                }
            }
            return true;
        });
    }

    static string MessageCodeWire(Bromelia.Domain.BroMessage m) => Bromelia.Domain.MessageCode.Wire(m.Code);
}
