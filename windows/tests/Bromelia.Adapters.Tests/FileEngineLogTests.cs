using Bromelia.Domain;
using Bromelia.Foundation;
using Xunit;
using static Bromelia.Tests.Fixtures;

namespace Bromelia.Adapters.Tests;

/// <summary>shared/fixtures/adapters/engine-log.cases.json.</summary>
public sealed class FileEngineLogTests : IDisposable
{
    readonly string _base = Path.Combine(Path.GetTempPath(), "bromelia-engine-log-" + Guid.NewGuid().ToString("N"));
    int _case;

    public void Dispose()
    {
        try { Directory.Delete(_base, true); } catch (IOException) { }
    }

    [Fact]
    public void TheSharedCasesPass()
    {
        RunCases("adapters/engine-log.cases.json", (id, given, expect) =>
        {
            var root = Path.Combine(_base, (++_case).ToString());
            var logs = Path.Combine(root, "data", "logs");
            var path = Path.Combine(logs, "engine.log");
            Directory.CreateDirectory(given["noLogsFolder"]?.AsBool == true ? Path.Combine(root, "data") : logs);
            if (given["before"]?.AsString is { } before) File.WriteAllText(path, before);
            if (given["logIsAFolder"]?.AsBool == true) Directory.CreateDirectory(path);
            var clock = new Bromelia.TestKit.ManualClock();
            var log = new FileEngineLog(new DiskFileSystem(), clock, path);
            foreach (var line in given["lines"]!.AsArray!)
            {
                clock.Advance(line["at"]!.AsInteger!.Value - clock.Now().UnixMilliseconds);
                log.Log(new BroMessage(MessageCode.Parse(line["code"]!.AsString!)!.Value,
                    line["params"] as JsonValue.Object ?? new JsonValue.Object(new List<KeyValuePair<string, JsonValue>>()),
                    EnumWire.Parse<Severity>(line["severity"]!.AsString)!.Value));
            }
            if (expect["logIsAFolder"]?.AsBool == true) Assert.True(Directory.Exists(path), "engine.log is still a folder");
            if (expect["file"]?.AsString is { } file) Assert.Equal(file, File.ReadAllText(path));
            if (expect["mode"]?.AsInteger is { } mode && !OperatingSystem.IsWindows()) Assert.Equal(mode, (long)File.GetUnixFileMode(path));
            return true;
        });
    }
}
