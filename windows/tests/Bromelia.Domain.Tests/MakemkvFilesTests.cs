using System.Xml;
using Bromelia.Domain;
using Bromelia.Foundation;
using Xunit;
using static Bromelia.Tests.Fixtures;

namespace Bromelia.Domain.Tests;

public class MakemkvFilesTests
{
    private static Dictionary<string, string> Map(JsonValue? v) =>
        (v?.AsObject ?? new List<KeyValuePair<string, JsonValue>>()).ToDictionary(m => m.Key, m => m.Value.AsString!);

    private static JsonValue MapJson(IReadOnlyDictionary<string, string> d) =>
        JsonValue.Of(d.OrderBy(kv => kv.Key, StringComparer.Ordinal).Select(kv => (kv.Key, JsonValue.Of(kv.Value))).ToArray());

    private static MakemkvSource SourceOf(JsonValue s) =>
        s["drive"] is { } d ? new MakemkvSource.Drive((int)d["index"]!.AsInteger!, d["device"]?.AsString ?? "")
        : s["iso"] is { } iso ? new MakemkvSource.Iso(iso.AsString!)
        : new MakemkvSource.File(s["folder"]!.AsString!);

    /// <summary>The argument makemkvcon gets for a source (the last one of info).</summary>
    private static string SourceArgument(MakemkvSource s) => MakemkvArgs.Info(s, new MakemkvOptions())[^1];

    [Fact]
    public void MakemkvFilesCases() => RunCases("domain/makemkv-files.cases.json", (id, given, expect) =>
    {
        if (given["text"] is { } text)
        {
            var parsed = SettingsConf.Parse(text.AsString!);
            Same(Sorted(expect["parse"]!), MapJson(parsed), "parse");
            if (expect["roundTrips"]?.AsBool == true) Same(MapJson(parsed), MapJson(SettingsConf.Parse(SettingsConf.Render(parsed, "Test"))), "round trip");
            return true;
        }
        if (given["generated"] is { } generated)
        {
            var xml = ProfileXml.Render(generated);
            if (expect["wellFormedXml"]?.AsBool == true) new XmlDocument().LoadXml(xml);
            foreach (var c in expect["contains"]?.AsArray ?? new List<JsonValue>()) Assert.Contains(c.AsString!, xml);
            if (expect["containsMakemkvDefaultSelection"]?.AsBool == true) Assert.Contains("app_DefaultSelectionString=\"" + ProfileXml.MakemkvDefaultSelection + "\"", xml);
            return true;
        }
        if (given["global"] is { } global)
        {
            var merged = SettingsLayers.Merge(Map(global), Map(given["profile"]), Map(given["drive"]), given["registrationKey"]?.AsString,
                given["selectionOverride"]?.AsString, given["makemkvDataDir"]?.AsString);
            Same(Sorted(expect["settings"]!), MapJson(merged), "settings");
            return true;
        }
        if (given["command"] is { } command)
        {
            var o = given["options"];
            var options = new MakemkvOptions(given["profilePath"]?.AsString, (int?)o?["minLengthSeconds"]?.AsInteger, (int?)o?["cacheMB"]?.AsInteger, o?["directIO"]?.AsBool);
            List<string> args;
            try
            {
                args = command.AsString switch
                {
                    "info" => MakemkvArgs.Info(SourceOf(given["source"]!), options),
                    "mkv" => MakemkvArgs.Mkv(SourceOf(given["source"]!), given["title"]!.AsString!, given["destination"]!.AsString!, options),
                    "backup" => MakemkvArgs.Backup(SourceOf(given["source"]!), given["decrypt"]!.AsBool!.Value, given["destination"]!.AsString!, options),
                    _ => MakemkvArgs.ScanDrives(),
                };
            }
            catch (BroFailure e)
            {
                Same(expect["error"]?["code"]?.AsString, e.Error.Code, "error");
                return true;
            }
            Assert.Null(expect["error"]);
            if (expect["arguments"] is { } exact) Same(exact, JsonValue.Of(args.Select(JsonValue.Of)), "arguments");
            if (expect["endsWith"] is { } end)
                Same(end, JsonValue.Of(args.Skip(args.Count - end.AsArray!.Count).Select(JsonValue.Of)), "endsWith");
            foreach (var c in expect["contains"]?.AsArray ?? new List<JsonValue>()) Assert.Contains(c.AsString!, args);
            return true;
        }
        if (given["path"] is { } path)
        {
            Same(expect["source"]!.AsString, SourceArgument(SourceResolver.Source(path.AsString!, given["isDirectory"]!.AsBool!.Value)), "source");
            return true;
        }
        if (given["html"] is { } || given["htmlFile"] is { })
        {
            var html = given["html"]?.AsString ?? Text(given["htmlFile"]!.AsString!);
            Same(expect["key"]!.AsString, BetaKeyPage.Parse(html), "key");
            return true;
        }
        if (given.AsObject!.Any(m => m.Key == "currentKey"))
        {
            Same(expect["mayReplace"]!.AsBool, BetaKey.MayReplace(given["currentKey"]!.AsString), "mayReplace");
            return true;
        }
        return false;
    });
}
