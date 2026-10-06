using Bromelia.Foundation;
using Bromelia.Ports;
using Xunit;
using static Bromelia.Tests.Fixtures;

namespace Bromelia.Adapters.Tests;

/// <summary>shared/fixtures/adapters/settings-isolation.cases.json.</summary>
public sealed class HomeDirIsolationTests : IDisposable
{
    readonly string _base = Path.Combine(Path.GetTempPath(), "bromelia-home-" + Guid.NewGuid().ToString("N"));
    /// <summary>A new folder for each case: deleting a folder and making it again under the same name can leave the
    /// Windows SMB client answering for the old one.</summary>
    string _root;
    int _case;

    public HomeDirIsolationTests() => _root = Path.Combine(_base, "0");

    public void Dispose()
    {
        try { Directory.Delete(_base, true); } catch (IOException) { }
    }

    string P(string relative) => Path.Combine(_root, relative.Replace('/', Path.DirectorySeparatorChar));

    string Shown(string? path) => path is null ? "(none)" : "<root>/" + Path.GetRelativePath(_root, path).Replace('\\', '/');

    [Fact]
    public void TheSharedCasesPass()
    {
        RunCases("adapters/settings-isolation.cases.json", (id, given, expect) =>
        {
            _root = Path.Combine(_base, (++_case).ToString());
            Directory.CreateDirectory(_root);
            foreach (var m in given["before"]?.AsObject ?? new List<KeyValuePair<string, JsonValue>>())
            {
                Directory.CreateDirectory(Path.GetDirectoryName(P(m.Key))!);
                File.WriteAllText(P(m.Key), m.Value.AsString);
            }
            var layout = EnumWire.Parse<HomeLayout>(given["layout"]!.AsString)!.Value;
            var settings = given["settings"]!.AsObject!.ToDictionary(m => m.Key, m => m.Value.AsString!);
            var run = new MakemkvRunSettings(settings, given["profileXml"]!.AsString, "/unused", P(given["workDirectory"]!.AsString!));
            var lease = new HomeDirIsolation(new DiskFileSystem(), layout).Prepare(run);
            lease.FirstOutput();
            foreach (var m in given["duringRun"]?.AsObject ?? new List<KeyValuePair<string, JsonValue>>()) File.WriteAllText(P(m.Key), m.Value.AsString);
            var work = P(given["workDirectory"]!.AsString!);
            void SameFiles(JsonValue want, string when)
            {
                var files = Directory.EnumerateFiles(work, "*", SearchOption.AllDirectories)
                    .Select(f => Path.GetRelativePath(_root, f).Replace('\\', '/')).OrderBy(f => f, StringComparer.Ordinal).ToList();
                Assert.Equal(want.AsObject!.Select(m => m.Key).OrderBy(f => f, StringComparer.Ordinal), files);
                foreach (var m in want.AsObject!) Assert.True(m.Value.AsString == File.ReadAllText(P(m.Key)), when + ": " + m.Key);
            }
            SameFiles(expect["files"]!, "while running");
            lease.Release();
            SameFiles(expect["afterRelease"] ?? expect["files"]!, "after release");

            var env = lease.Environment();
            foreach (var m in expect["environment"]!.AsObject!) Assert.Equal(m.Value.AsString, Shown(env[m.Key]));
            Assert.Equal(expect["profilePath"]!.AsString ?? "(none)", Shown(lease.ProfilePath()));
            if (!OperatingSystem.IsWindows())
                foreach (var m in expect["modes"]?.AsObject ?? new List<KeyValuePair<string, JsonValue>>())
                    Assert.Equal(m.Value.AsInteger, (long)File.GetUnixFileMode(P(m.Key)));
            return true;
        });
    }
}
