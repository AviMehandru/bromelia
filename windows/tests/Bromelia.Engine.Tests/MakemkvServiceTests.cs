using System.Text;
using System.Text.RegularExpressions;
using Bromelia.Domain;
using Bromelia.Foundation;
using Bromelia.Ports;
using Xunit;
using static Bromelia.Tests.Fixtures;

namespace Bromelia.Engine.Tests;

/// <summary>shared/fixtures/engine/makemkv-service.cases.json: MakemkvService.Invocation behind a real ConfigService
/// (a fake system keystore, an empty secrets folder), on a real executor.</summary>
public sealed class MakemkvServiceTests
{
    static readonly List<KeyValuePair<string, JsonValue>> None = new();

    static JsonValue Number(int? n) => n is { } v ? JsonValue.Of(v) : JsonValue.Null.Instance;

    static JsonValue Projection(MakemkvInvocation i)
    {
        JsonValue? xml = null;
        if (i.Settings.ProfileXml is { } text)
            xml = JsonValue.Of(("name", JsonValue.Of(Regex.Match(text, "<name lang=\"eng\">(.*?)</name>").Groups[1].Value)),
                ("selection", JsonValue.Of(Regex.Match(text, "app_DefaultSelectionString=\"(.*?)\"").Groups[1].Value)));
        var o = i.Options;
        return JsonValue.Of(
            ("settings", JsonValue.Of(i.Settings.Settings.OrderBy(kv => kv.Key, StringComparer.Ordinal).Select(kv => (kv.Key, JsonValue.Of(kv.Value))).ToArray())),
            ("profileXml", xml ?? JsonValue.Null.Instance),
            ("options", JsonValue.Of(("profilePath", JsonValue.Of(o.ProfilePath)), ("minLengthSeconds", Number(o.MinLengthSeconds)),
                ("cacheMB", Number(o.CacheMB)), ("directIO", o.DirectIO is { } d ? JsonValue.Of(d) : JsonValue.Null.Instance), ("extraArguments", JsonValue.Of(o.ExtraArguments)),
                ("scan", JsonValue.Of(o.Scan)))),
            ("workDirectory", JsonValue.Of(i.Settings.WorkDirectory)),
            ("dataDir", JsonValue.Of(i.Settings.DataDir)),
            ("stallTimeout", i.StallTimeout is { } s ? JsonValue.Of((long)s.Seconds) : JsonValue.Null.Instance),
            ("transcript", JsonValue.Of(i.Transcript)));
    }

    /// <summary>The expectation with its settings in key order, so maps compare as maps.</summary>
    static JsonValue Sorted(JsonValue expect) => JsonValue.Of(expect.AsObject!.Select(m => (m.Key, m.Key == "settings"
        ? JsonValue.Of(m.Value.AsObject!.OrderBy(kv => kv.Key, StringComparer.Ordinal).Select(kv => (kv.Key, kv.Value)).ToArray())
        : m.Value)).ToArray());

    [Fact]
    public void TheSharedCasesPass()
    {
        RunCases("engine/makemkv-service.cases.json", (id, given, expect) =>
        {
            using var w = new EngineClockTests.World();
            var bus = new EventBus(w.Clock);
            var problems = new ProblemService(bus, w.Clock);
            var keystore = new ConfigServiceTests.Keystore();
            foreach (var (k, v) in given["secrets"]?.AsObject ?? None) keystore.Values[k] = v.AsString!;
            foreach (var (k, v) in given["keystoreFails"]?.AsObject ?? None) keystore.Fails[k] = v;
            var environment = (given["environment"]?.AsObject ?? None).ToDictionary(m => m.Key, m => m.Value.AsString!);
            var fs = new MemoryFileSystem();
            var config = new ConfigService(w.Executor, bus, problems, fs, keystore, new ConfigServiceTests.Keystore { SystemStore = false },
                new ConfigServiceTests.Kv(), "/data/config.json", environment);

            var dataDir = given["dataDir"]?.AsString ?? "/home/u/.MakeMKV";
            string? userSettings = dataDir + "/settings.conf";
            if (given["userSettings"] is { IsNull: true }) userSettings = null;
            else if (given["userSettings"]?.AsString is { } text)
            {
                fs.CreateDirectory(dataDir, parentsMustExist: false);
                fs.WriteAtomically(userSettings, Encoding.UTF8.GetBytes(text), 0x180);
            }
            if (given["userSettingsFails"] is { } fails)
                fs.ReadFails[userSettings!] = new BroError(fails["code"]!.AsString!, (JsonValue.Object)fails["params"]!);
            var service = new MakemkvService(fs, config, dataDir, userSettings, "/home/u");

            var doc = ConfigCodec.Decode(JsonValue.EncodeCanonical(given["config"]!));
            var profile = ProfileResolver.Resolve(doc, given["configId"]?.AsString, new RuleFacts("", "", ""), null);
            JsonValue got;
            try
            {
                got = Projection(service.Invocation(doc, profile, given["selectionOverride"]?.AsString, given["jobDirectory"]?.AsString ?? "/data/jobs/j1"));
            }
            catch (BroFailure f)
            {
                got = JsonValue.Of(("error", JsonValue.Of(("code", JsonValue.Of(f.Error.Code)), ("params", f.Error.Params))));
            }
            var want = Encoding.UTF8.GetString(JsonValue.EncodeCanonical(Sorted(expect)));
            var have = Encoding.UTF8.GetString(JsonValue.EncodeCanonical(got));
            Assert.True(want == have, id + ": expected\n" + want + "got\n" + have);
            return true;
        });
    }
}
