using Bromelia.Foundation;
using Bromelia.Ports;
using Xunit;
using static Bromelia.Tests.Fixtures;

namespace Bromelia.Adapters.Tests;

/// <summary>shared/fixtures/adapters/settings-isolation.cases.json.</summary>
public sealed class HomeDirIsolationTests : IDisposable
{
    readonly string _root = Path.Combine(Path.GetTempPath(), "bromelia-home-" + Guid.NewGuid().ToString("N"));

    public void Dispose()
    {
        try { Directory.Delete(_root, true); } catch (IOException) { }
    }

    string P(string relative) => Path.Combine(_root, relative.Replace('/', Path.DirectorySeparatorChar));

    string Shown(string? path) => path is null ? "(none)" : "<root>/" + Path.GetRelativePath(_root, path).Replace('\\', '/');

    [Fact]
    public void TheSharedCasesPass()
    {
        RunCases("adapters/settings-isolation.cases.json", (id, given, expect) =>
        {
            if (Directory.Exists(_root)) Directory.Delete(_root, true);
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
            lease.Release();

            var env = lease.Environment();
            foreach (var m in expect["environment"]!.AsObject!) Assert.Equal(m.Value.AsString, Shown(env[m.Key]));
            Assert.Equal(expect["profilePath"]!.AsString ?? "(none)", Shown(lease.ProfilePath()));
            var work = P(given["workDirectory"]!.AsString!);
            var files = Directory.EnumerateFiles(work, "*", SearchOption.AllDirectories)
                .Select(f => Path.GetRelativePath(_root, f).Replace('\\', '/')).OrderBy(f => f, StringComparer.Ordinal).ToList();
            Assert.Equal(expect["files"]!.AsObject!.Select(m => m.Key).OrderBy(f => f, StringComparer.Ordinal), files);
            foreach (var m in expect["files"]!.AsObject!) Assert.Equal(m.Value.AsString, File.ReadAllText(P(m.Key)));
            if (!OperatingSystem.IsWindows())
                foreach (var m in expect["modes"]?.AsObject ?? new List<KeyValuePair<string, JsonValue>>())
                    Assert.Equal(m.Value.AsInteger, (long)File.GetUnixFileMode(P(m.Key)));
            return true;
        });
    }
}
