using System.Runtime.Versioning;
using System.Text;
using Bromelia.Domain;
using Bromelia.Foundation;
using Bromelia.Ports;
using Xunit;
using static Bromelia.Tests.Fixtures;

namespace Bromelia.Adapters.Windows.Tests;

/// <summary>shared/fixtures/adapters/file-system.cases.json, and the file-system cases of
/// platform-adapters.contract.json, on real Windows.</summary>
[SupportedOSPlatform("windows")]
public sealed class PlatformFileSystemTests : IDisposable
{
    readonly string _base = Path.Combine(Path.GetTempPath(), "bromelia-fs-" + Guid.NewGuid().ToString("N"));
    /// <summary>A new folder for each case: deleting a folder and making it again under the same name can leave the
    /// Windows SMB client answering for the old one.</summary>
    string _root;
    int _case;
    readonly PlatformFileSystem _fs = new();

    public PlatformFileSystemTests() => _root = Path.Combine(_base, "0");

    public void Dispose()
    {
        try { if (Directory.Exists(_base)) Directory.Delete(_base, true); } catch (IOException) { }
    }

    string P(string relative) => relative == "." ? _root : Path.Combine(_root, relative.Replace('/', '\\'));

    string Rel(string path) => Path.GetRelativePath(_root, path).Replace('\\', '/');

    JsonValue Pair(MovedItem m) => JsonValue.Of(new[] { JsonValue.Of(Rel(m.From)), JsonValue.Of(Rel(m.To)) });

    /// <summary>What moveMerging's onMoved heard, in order.</summary>
    readonly List<MovedItem> _reported = new();

    void Build(JsonValue tree)
    {
        _root = Path.Combine(_base, (++_case).ToString());
        Directory.CreateDirectory(_root);
        foreach (var (name, content) in tree.AsObject!.Select(m => (m.Key, m.Value)))
            if (name.EndsWith('/')) Directory.CreateDirectory(P(name.TrimEnd('/')));
            else File.WriteAllText(P(name), content.AsString!, new UTF8Encoding(false));
    }

    /// <summary>The tree under the root, in the cases' notation, sorted.</summary>
    SortedDictionary<string, string?> Tree()
    {
        var tree = new SortedDictionary<string, string?>(StringComparer.Ordinal);
        foreach (var d in Directory.EnumerateDirectories(_root, "*", SearchOption.AllDirectories)) tree[Rel(d) + "/"] = null;
        foreach (var f in Directory.EnumerateFiles(_root, "*", SearchOption.AllDirectories)) tree[Rel(f)] = File.ReadAllText(f);
        return tree;
    }

    JsonValue Run(JsonValue[] op)
    {
        string S(int i) => op[i].AsString!;
        switch (S(0))
        {
            case "exists": return JsonValue.Of(_fs.Exists(P(S(1))));
            case "stat":
                var info = _fs.Stat(P(S(1)));
                return info.IsDirectory ? JsonValue.Of(("isDirectory", JsonValue.Of(true)))
                    : JsonValue.Of(("size", JsonValue.Of(info.Size)), ("isDirectory", JsonValue.Of(false)));
            case "list":
                return JsonValue.Of(_fs.List(P(S(1))).Select(e => e.Name + (e.IsDirectory ? "/" : "")).OrderBy(n => n, StringComparer.Ordinal).Select(JsonValue.Of));
            case "read": return JsonValue.Of(Encoding.UTF8.GetString(_fs.Read(P(S(1)))));
            case "readRange": return JsonValue.Of(Encoding.UTF8.GetString(_fs.ReadRange(P(S(1)), op[2].AsInteger!.Value, (int)op[3].AsInteger!.Value)));
            case "createDirectory": _fs.CreateDirectory(P(S(1)), op[2].AsBool!.Value); return JsonValue.Null.Instance;
            case "writeAtomically": _fs.WriteAtomically(P(S(1)), Encoding.UTF8.GetBytes(S(2)), (int)op[3].AsInteger!.Value); return JsonValue.Null.Instance;
            case "rename": _fs.Rename(P(S(1)), P(S(2))); return JsonValue.Null.Instance;
            case "moveMerging":
                var result = _fs.MoveMerging(P(S(1)), P(S(2)), EnumWire.Parse<MovePolicy>(S(3))!.Value, _reported.Add);
                Assert.Equal(result.OrderBy(m => m.From, StringComparer.Ordinal), _reported.OrderBy(m => m.From, StringComparer.Ordinal));
                return JsonValue.Of(result.Select(Pair));
            case "remove": _fs.Remove(P(S(1))); return JsonValue.Null.Instance;
            case "moveToTrash": _fs.MoveToTrash(P(S(1)), P(S(2))); return JsonValue.Null.Instance;
            case "syncFile": _fs.SyncFile(P(S(1))); return JsonValue.Null.Instance;
            case "syncDirectory": _fs.SyncDirectory(P(S(1))); return JsonValue.Null.Instance;
            case "openForReading":
                var stream = _fs.OpenForReading(P(S(1)), op[2].AsBool!.Value);
                var all = new List<byte>();
                for (byte[] chunk; (chunk = stream.Read(4)).Length > 0;) all.AddRange(chunk);
                stream.Close();
                return JsonValue.Of(Encoding.UTF8.GetString(all.ToArray()));
        }
        throw new InvalidOperationException("unknown operation " + S(0));
    }

    [Fact]
    public void TheSharedCasesPass()
    {
        if (!OperatingSystem.IsWindows()) return;
        RunCases("adapters/file-system.cases.json", (id, given, expect) =>
        {
            Build(given["tree"]!);
            _reported.Clear();
            var op = given["op"]!.AsArray!.ToArray();
            // A file held open without delete sharing can't be moved.
            using var locked = given["locked"]?.AsString is { } l ? new FileStream(P(l), FileMode.Open, FileAccess.Read, FileShare.Read) : null;
            if (expect["error"] is { } error)
            {
                var e = Assert.Throws<BroFailure>(() => Run(op));
                if (expect["movedBeforeTheError"] is { } before) Assert.Equal(before, JsonValue.Of(_reported.Select(Pair)));
                Assert.Equal(error["code"]!.AsString, e.Error.Code);
                foreach (var (key, value) in error["params"]!.AsObject!.Select(m => (m.Key, m.Value)))
                {
                    var actual = e.Error.Params[key]!.AsString!;
                    if (key == "path") actual = "<root>/" + Rel(actual);
                    Assert.Equal(value.AsString, actual);
                }
            }
            else
            {
                var result = Run(op);
                if (expect["result"] is { } want)
                {
                    if (want.AsObject is { } members && result.AsObject is { } got)
                        foreach (var m in members) Assert.Equal(m.Value, got.First(g => g.Key == m.Key).Value);
                    else
                        Assert.Equal(want, result);
                }
            }
            if (expect["tree"] is { } tree)
            {
                var wantTree = tree.AsObject!.OrderBy(m => m.Key, StringComparer.Ordinal).Select(m => m.Key + " = " + (m.Value.AsString ?? "(folder)"));
                Assert.Equal(string.Join("\n", wantTree), string.Join("\n", Tree().Select(kv => kv.Key + " = " + (kv.Value ?? "(folder)"))));
            }
            return true;
        });
    }

    [Fact] // file-system-durable-write, file-system-read-uncached
    public void WritesAreDurableAndReadsCanBypassTheCache()
    {
        if (!OperatingSystem.IsWindows()) return;
        Directory.CreateDirectory(_root);
        var big = new byte[3 * (1 << 20) + 123];
        new Random(7).NextBytes(big);
        var path = P("big.bin");
        _fs.WriteAtomically(path, big, 0x1A4);
        _fs.SyncFile(path);
        _fs.SyncDirectory(_root);
        var stream = _fs.OpenForReading(path, bypassCache: true);
        var all = new List<byte>();
        for (byte[] chunk; (chunk = stream.Read(100_000)).Length > 0;) all.AddRange(chunk);
        stream.Close();
        Assert.Equal(big, all.ToArray());
        Assert.Equal(new[] { "big.bin" }, Directory.EnumerateFileSystemEntries(_root).Select(Path.GetFileName));
    }

    [Fact]
    public void TheVolumeHasAnIdFreeSpaceAndAType()
    {
        if (!OperatingSystem.IsWindows()) return;
        Directory.CreateDirectory(_root);
        var v = _fs.Volume(P("not/yet/there"));
        Assert.Matches("^[0-9A-F]{8}$", v.Id);
        Assert.True(v.FreeBytes > 0);
        Assert.Equal("NTFS", v.FsType);
        Assert.False(v.CaseSensitive);
        Assert.Equal(v.Id, _fs.Volume(_root).Id);
    }
}
