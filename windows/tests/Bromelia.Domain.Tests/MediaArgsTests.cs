using Bromelia.Domain;
using Bromelia.Foundation;
using Xunit;
using static Bromelia.Tests.Fixtures;

namespace Bromelia.Domain.Tests;

public class MediaArgsTests
{
    private static List<string> Strings(JsonValue? v) => (v?.AsArray ?? new List<JsonValue>()).Select(x => x.AsString!).ToList();

    private static JsonValue StringsJson(IEnumerable<string> s) => JsonValue.Of(s.Select(JsonValue.Of));

    private static IReadOnlyDictionary<string, string> Values(JsonValue? v) =>
        (v?.AsObject ?? new List<KeyValuePair<string, JsonValue>>()).ToDictionary(m => m.Key, m => m.Value.AsString!);

    private static void Near(JsonValue expected, double actual, string what)
    {
        if (Math.Abs(expected.AsNumber!.Value - actual) > 1e-9) throw new Xunit.Sdk.XunitException($"{what}: expected {expected}, got {actual}");
    }

    private static MkvProbe ProbeOf(JsonValue p) => new(p["durationSeconds"]?.AsNumber,
        Strings(p["trackTypes"]).Select((t, i) => new MkvTrack(i, t)).ToList(), (int)(p["chapterCount"]?.AsInteger ?? 0));

    private static Title TitleWithTracks(JsonValue t, IEnumerable<TrackKind> kinds) => TitlesTests.TitleOf(t["index"] == null
        ? JsonValue.Of(new[] { ("index", JsonValue.Of(0)) }.Concat(t.AsObject!.Select(m => (m.Key, m.Value))).ToArray()) : t) with
    {
        Tracks = kinds.Select((k, i) => new Track(i, k, "", "", "", "", false, new Dictionary<int, string>())).ToList(),
    };

    private static StepDefinition Step(JsonValue given, string kind = "command") =>
        StepDefinition.Decode(JsonValue.Of(new[] { ("id", JsonValue.Of("s")), ("kind", JsonValue.Of(kind)) }
            .Concat(given.AsObject!.Where(m => m.Key != "kind").Select(m => (m.Key, m.Value))).ToArray()));

    [Fact]
    public void MediaArgsCases() => RunCases("domain/media-args.cases.json", (id, given, expect) =>
    {
        if (id.StartsWith("probe-"))
        {
            var p = MkvProbe.Parse(given["json"]!.AsString!);
            var want = expect["probe"]!;
            if (want is JsonValue.Null) { Assert.Null(p); return true; }
            Near(want["durationSeconds"]!, p!.DurationSeconds!.Value, "durationSeconds");
            Same(want["trackTypes"], StringsJson(p.Tracks.Select(t => t.Type)), "trackTypes");
            Same(want["chapterCount"]!.AsInteger, (long)p.ChapterCount, "chapterCount");
            return true;
        }
        if (given["expectedSeconds"] is { } secs)
        {
            var want = expect["tolerance"]!.AsArray!;
            for (int i = 0; i < want.Count; i++) Near(want[i], RipCheck.Tolerance(secs.AsArray![i].AsNumber!.Value).Seconds, "tolerance");
            return true;
        }
        if (id.StartsWith("check-"))
        {
            var t = given["title"]!;
            var kinds = Strings(t["tracks"]).Select(k => EnumWire.Parse<TrackKind>(k.ToLowerInvariant())!.Value);
            var r = RipCheck.Check(ProbeOf(given["probe"]!), TitleWithTracks(JsonValue.Of(t.AsObject!.Where(m => m.Key != "tracks").Select(m => (m.Key, m.Value)).ToArray()), kinds));
            if (expect["problems"] is { } problems) Same(problems, JsonValue.Of(r.Problems.Select(m => m.ToJson())), "problems");
            if (expect["notes"] is { } notes) Same(notes, JsonValue.Of(r.Notes.Select(m => m.ToJson())), "notes");
            if (expect["problemCodes"] is { } pc) Same(pc, StringsJson(r.Problems.Select(m => MessageCode.Wire(m.Code))), "problemCodes");
            if (expect["noteCodes"] is { } nc) Same(nc, StringsJson(r.Notes.Select(m => MessageCode.Wire(m.Code))), "noteCodes");
            return true;
        }
        if (id.StartsWith("remux-"))
        {
            var layout = given["layout"]!.AsArray!.Select(l => new MkvTrack((int)l["id"]!.AsInteger!, l["type"]!.AsString!)).ToList();
            Title title = given["listing"] is { } listing
                ? ListingTests.ListingOf(Text(listing.AsString!)).Titles.First(x => x.Index == given["title"]!.AsInteger)
                : TitleWithTracks(JsonValue.Of(), Strings(given["tracks"]).Select(k => k == "subtitles" ? TrackKind.Subtitle : EnumWire.Parse<TrackKind>(k)!.Value));
            var keep = given["keep"]!.AsArray!.Select(k => (int)k.AsInteger!).ToList();
            var args = Remux.Arguments(layout, title, keep, given["input"]!.AsString!, given["output"]!.AsString!);
            Same(expect["arguments"], args == null ? JsonValue.Null.Instance : StringsJson(args), "arguments");
            return true;
        }
        if (id == "split-arguments")
        {
            var chapters = given["chapters"]!.AsArray!.Select(c => (int)c.AsInteger!).ToList();
            Same(expect["arguments"], StringsJson(Split.Arguments(chapters, given["input"]!.AsString!, given["output"]!.AsString!)), "arguments");
            return true;
        }
        if (id == "simple-chapters")
        {
            var starts = SimpleChapters.Parse(given["text"]!.AsString!);
            var want = expect["starts"]!.AsArray!;
            Assert.Equal(want.Count, starts.Count);
            for (int i = 0; i < want.Count; i++) Near(want[i], starts[i].Seconds, "start");
            return true;
        }
        if (id.StartsWith("handbrake-"))
        {
            if (given["files"] is { } files)
            {
                Same(expect["sources"], StringsJson(Strings(files).Where(HandBrakeArgs.IsSource)), "sources");
                return true;
            }
            if (given["lines"] is { } lines)
            {
                var filter = new ProgressFilter();
                Same(expect["kept"], StringsJson(Strings(lines).Where(l => HandBrakeArgs.KeepLine(ref filter, l)).ToList()), "kept");
                return true;
            }
            var step = Step(given["step"]!, "handbrake");
            if (expect["preset"] is { } preset)
            {
                Same(expect["background"]!.AsBool, step.Background, "background");
                Same(preset.AsString, step.Handbrake!.Preset, "preset");
                foreach (var m in expect["runsOn"]!.AsObject!)
                    Same(m.Value.AsBool, StepFilter.Applies(step, EnumWire.Parse<Outcome>(m.Key)!.Value), m.Key);
            }
            if (expect["output"] is { } output) Same(output.AsString, HandBrakeArgs.Output(step, Values(given["values"]), null), "output");
            if (expect["arguments"] is { } arguments)
                Same(arguments, StringsJson(HandBrakeArgs.Build(step, given["input"]!.AsString!, given["output"]!.AsString!, null)), "arguments");
            return true;
        }
        if (id.StartsWith("backup-structure"))
        {
            bool iso = given["iso"]?.AsBool == true;
            var entries = given["folder"] is { } folder ? Strings(folder) : null;
            byte[]? header = null;
            if (given["bytes"]?.AsInteger is { } size)
            {
                // The file's bytes from 32769: the descriptor, else zeros.
                var descriptor = given["descriptorAt32769"]?.AsString ?? "";
                header = new byte[Math.Max(0, Math.Min(5, (int)size - 32769))];
                for (int i = 0; i < header.Length && i < descriptor.Length; i++) header[i] = (byte)descriptor[i];
            }
            var problem = BackupStructure.Problem("disc", iso, entries, header);
            var want = expect["problem"]!;
            Same(want is JsonValue.Null ? null : want["code"]!.AsString, problem == null ? null : MessageCode.Wire(problem.Code), "problem");
            return true;
        }
        return false;
    });

    [Fact]
    public void CommandCases() => RunCases("domain/arguments.cases.json", id => id.StartsWith("invocation"), (id, given, expect) =>
    {
        var line = CommandArgs.Build(Step(given["step"]!), Values(given["values"]), Strings(given["files"]), given["home"]?.AsString);
        Same(expect["executable"]!.AsString, line.Executable, "executable");
        Same(expect["arguments"], StringsJson(line.Arguments), "arguments");
        return true;
    });

    [Fact]
    public void OtherToolArguments()
    {
        Assert.Equal(new[] { "-t", "T", "-b", "B", "tgram://bot/chat" }, AppriseArgs.Build("tgram://bot/chat", "T", "B"));
        var none = JsonValue.Of();
        Assert.Equal(new CommandLine("cyanrip", new[] { "-d", "/dev/sr0", "-o", "flac" }).Arguments,
            CdRipperArgs.Build(none, "/dev/sr0", new[] { "abcde", "cyanrip" })!.Arguments);
        Assert.Equal("abcde", CdRipperArgs.Build(none, "/dev/sr0", new[] { "abcde" })!.Executable);
        Assert.Null(CdRipperArgs.Build(none, "/dev/sr0", Array.Empty<string>()));
        var custom = CdRipperArgs.Build(JsonValue.Of(("audioCommand", JsonValue.Of("whipper cd -d {device} rip"))), "/dev/sr1", Array.Empty<string>())!;
        Assert.Equal("whipper", custom.Executable);
        Assert.Equal(new[] { "cd", "-d", "/dev/sr1", "rip" }, custom.Arguments);
        // A leading ~ is the home folder in a preset file too.
        var step = StepDefinition.Decode(JsonValue.Parse("""{"id": "h", "kind": "handbrake", "handbrake": {"presetFile": "~/p.json", "preset": ""}}""")!);
        Assert.Equal(new[] { "--preset-import-file", "/home/me/p.json", "-i", "/i", "-o", "/o" }, HandBrakeArgs.Build(step, "/i", "/o", "/home/me/"));
    }
}
