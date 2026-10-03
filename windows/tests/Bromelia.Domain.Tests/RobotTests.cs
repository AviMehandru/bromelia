using Bromelia.Domain;
using Bromelia.Foundation;
using Xunit;
using static Bromelia.Tests.Fixtures;

namespace Bromelia.Domain.Tests;

public class RobotTests
{
    /// <summary>An event as the fixtures write it.</summary>
    internal static JsonValue EventJson(RobotEvent? e) => e switch
    {
        null => JsonValue.Null.Instance,
        RobotEvent.Message { Value: var m } => JsonValue.Of(("kind", JsonValue.Of("message")), ("code", JsonValue.Of(m.Code)),
            ("flags", JsonValue.Of(m.Flags)), ("text", JsonValue.Of(m.Text)), ("format", JsonValue.Of(m.Format)),
            ("params", JsonValue.Of(m.Params.Select(JsonValue.Of))), ("severity", JsonValue.Of(EnumWire.Name(MessageCatalog.Severity(m))))),
        RobotEvent.ProgressValue v => JsonValue.Of(("kind", JsonValue.Of("progressValue")), ("current", JsonValue.Of(v.Current)),
            ("total", JsonValue.Of(v.Total)), ("max", JsonValue.Of(v.Max))),
        RobotEvent.ProgressTotal t => JsonValue.Of(("kind", JsonValue.Of("progressTotal")), ("code", JsonValue.Of(t.Code)),
            ("id", JsonValue.Of(t.Id)), ("name", JsonValue.Of(t.Name))),
        RobotEvent.ProgressCurrent c => JsonValue.Of(("kind", JsonValue.Of("progressCurrent")), ("code", JsonValue.Of(c.Code)),
            ("id", JsonValue.Of(c.Id)), ("name", JsonValue.Of(c.Name))),
        RobotEvent.TitleCount n => JsonValue.Of(("kind", JsonValue.Of("titleCount")), ("count", JsonValue.Of(n.Count))),
        RobotEvent.Raw r => JsonValue.Of(("kind", JsonValue.Of("raw")), ("text", JsonValue.Of(r.Text))),
        _ => JsonValue.Of(("kind", JsonValue.Of(e.GetType().Name))),
    };

    internal static RunAccumulator Accumulate(string fixture, bool readsData = false)
    {
        var acc = new RunAccumulator(readsData);
        foreach (var line in Text(fixture).Split('\n'))
            if (Robot.ParseLine(line) is { } e) acc.Feed(e);
        return acc;
    }

    private static RobotMessage Message(JsonValue m) =>
        new((int)m["code"]!.AsInteger!, (int)m["flags"]!.AsInteger!, 0, m["text"]!.AsString!, "", new List<string>());

    private static JsonValue NoticeJson(MakemkvNotice? n)
    {
        if (n is null) return JsonValue.Null.Instance;
        var kind = n.GetType().Name;
        var members = new List<(string, JsonValue)> { ("kind", JsonValue.Of(char.ToLowerInvariant(kind[0]) + kind.Substring(1))) };
        if (n is MakemkvNotice.LibreDrive l) members.Add(("detail", JsonValue.Of(l.Detail)));
        members.Add(("licenseProblem", JsonValue.Of(MakemkvNotice.IsLicenseProblem(n))));
        return JsonValue.Of(members.ToArray());
    }

    [Fact]
    public void RobotCases() => RunCases("domain/robot.cases.json", (id, given, expect) =>
    {
        if (given["fields"] is { } fields)
        {
            Same(expect["fields"], JsonValue.Of(Robot.SplitFields(fields.AsString!).Select(JsonValue.Of)), "fields");
            return true;
        }
        if (given["line"] is { } line)
        {
            var e = Robot.ParseLine(line.AsString!);
            if (expect["event"] is { } ev) Same(ev, EventJson(e), "event");
            else if (expect["notice"] is { } notice)
                Same(notice, NoticeJson(e is RobotEvent.Message m ? MessageCatalog.Notice(m.Value) : null), "notice");
            else return false;
            return true;
        }
        if (given["message"] is { } msg)
        {
            Same(expect["severity"]!.AsString, EnumWire.Name(MessageCatalog.Severity(Message(msg))), "severity");
            return true;
        }
        if (given["codes"] is { } codes)
        {
            foreach (var kind in new[] { "error", "warning" })
                foreach (var code in codes[kind]!.AsArray!)
                    Same(kind, EnumWire.Name(MessageCatalog.Severity(new RobotMessage((int)code.AsInteger!, (int)given["flags"]!.AsInteger!, 0, "x", "", new List<string>()))), $"severity of {code.AsInteger}");
            return true;
        }
        if (given["fixture"] is { } fixture)
        {
            var acc = Accumulate(fixture.AsString!);
            if (expect["lastSaved"] is { } saved) Same(saved.AsInteger, (long?)acc.Saved, "saved");
            if (expect["errorCodes"] is { } errorCodes)
                Same(errorCodes, JsonValue.Of(acc.Errors.Select(m => JsonValue.Of(m.Code))), "error codes");
            if (expect["lastErrorParams"] is { } lastParams)
                Same(lastParams, JsonValue.Of(acc.Errors.Last().Params.Select(JsonValue.Of)), "last error's params");
            return true;
        }
        if (given["fixtures"] is { } list)
        {
            foreach (var f in list.AsArray!)
            {
                var part = expect["firstErrorContains"]![f.AsString!]!.AsString!;
                var first = Accumulate(f.AsString!).FirstError;
                if (first is null || !first.Text.Contains(part)) throw new Xunit.Sdk.XunitException($"{f.AsString}: first error {first?.Text} doesn't contain {part}");
            }
            return true;
        }
        return false;
    });

    [Fact]
    public void DriveScanFixture()
    {
        var golden = Json("domain/drive-scan.drives.expected.json");
        var drives = Text(golden["input"]!.AsString!).Split('\n').Select(Robot.ParseLine)
            .Select(e => e is null ? null : MakemkvDrive.From(e)).Where(d => d != null).Select(d => d!).ToList();
        var actual = JsonValue.Of(drives.Select(d =>
        {
            var flags = new List<JsonValue>();
            if (d.Flags.DvdFiles) flags.Add(JsonValue.Of("dvdFiles"));
            if (d.Flags.HdDvdFiles) flags.Add(JsonValue.Of("hdDvdFiles"));
            if (d.Flags.BlurayFiles) flags.Add(JsonValue.Of("blurayFiles"));
            if (d.Flags.AacsFiles) flags.Add(JsonValue.Of("aacsFiles"));
            if (d.Flags.BdsvmFiles) flags.Add(JsonValue.Of("bdsvmFiles"));
            return JsonValue.Of(("index", JsonValue.Of(d.Index)), ("state", JsonValue.Of(EnumWire.Name(d.State))),
                ("present", JsonValue.Of(MakemkvDrive.IsPresent(d))), ("flags", JsonValue.Of(flags)),
                ("typeText", JsonValue.Of(DiscFlags.TypeText(d.Flags))), ("identification", JsonValue.Of(d.Identification)),
                ("label", JsonValue.Of(d.Label)), ("device", JsonValue.Of(d.Device)));
        }));
        Same(golden["expect"], actual, "drives");
    }

    internal static MakemkvDrive DriveOf(JsonValue d) =>
        new(0, DriveState.Inserted, new DiscFlags(0), d["identification"]!.AsString!, "", d["device"]!.AsString!);

    internal static DriveMatch MatchOf(JsonValue? m) =>
        new(m?["driveName"]?.AsString ?? "", m?["devicePath"]?.AsString ?? "");

    [Fact]
    public void DriveJoinCases()
    {
        RunCases("domain/rules.cases.json", id => id.StartsWith("drive-"), (id, given, expect) =>
        {
            if (expect["matches"] is { } matches)
                Same(matches.AsBool, DriveJoin.Matches(MatchOf(given["match"]), DriveOf(given["drive"]!)), "matches");
            else if (expect["entry"] is { } entry)
            {
                var drives = given["drives"]!.AsArray!.Select(d => new DriveEntry(d["id"]!.AsString!, MatchOf(d["match"]),
                    Enabled: d["enabled"]?.AsBool ?? true)).ToList();
                Same(entry.AsString, DriveJoin.EntryFor(drives, DriveOf(given["drive"]!))?.Id, "entry");
            }
            else if (expect["driveId"] is { } driveId)
                Same(driveId.AsString, DriveJoin.DriveId(given["identification"]!.AsString!, given["device"]!.AsString!), "drive id");
            else return false;
            return true;
        });
        RunCases("domain/identity.cases.json", id => id.StartsWith("drive-"), (id, given, expect) =>
        {
            Same(expect["shortModel"]!.AsString, DriveJoin.ShortModel(given["identification"]!.AsString!), "short model");
            return true;
        });
    }

    [Fact]
    public void JoinsMakemkvAndOsDrives()
    {
        var mk = new List<MakemkvDrive>
        {
            new(0, DriveState.Inserted, new DiscFlags(4), "BD-RE HL-DT-ST BD-RE  WH16NS60 1.02 KLAM6E84325", "MOVIE", "/dev/rdisk4"),
            new(1, DriveState.NoDrive, new DiscFlags(0), "", "", ""),
        };
        var os = new List<OsDrive> { new("/dev/disk5"), new("/dev/disk4", "HL-DT-ST", "/Volumes/MOVIE") };
        var joined = DriveJoin.Join(mk, os);
        Assert.Equal(2, joined.Count);
        Assert.Equal("drv-86920bcfd062b640", joined[0].DriveId);
        Assert.Equal("/Volumes/MOVIE", joined[0].Os?.MountPath);
        Assert.Null(joined[1].Makemkv);
        Assert.Equal(DriveJoin.DriveId("", "/dev/disk5"), joined[1].DriveId);
    }

    [Fact]
    public void AccumulatorStopsOnSpaceWarningAndRenumberedDrives()
    {
        var acc = new RunAccumulator(readsData: true, expectedIndex: 0, expectedDevice: "/dev/rdisk4");
        acc.Feed(Robot.ParseLine("DRV:0,2,999,12,\"BD-RE X\",\"DISC\",\"/dev/rdisk4\"")!);
        Assert.Null(acc.StopReason);
        acc.Feed(Robot.ParseLine("MSG:1004,0,1,\"Debug logging enabled, log will be saved as file:///Users/me/My%20Logs/MakeMKV_log.txt\",\"%1\",\"file:///Users/me/My%20Logs/MakeMKV_log.txt\"")!);
        Assert.Equal("/Users/me/My Logs/MakeMKV_log.txt", acc.DebugLog);
        acc.Feed(Robot.ParseLine("DRV:0,2,999,12,\"BD-RE X\",\"DISC\",\"/dev/rdisk5\"")!);
        Assert.Equal(MessageCode.DriveRenumbered, acc.StopReason?.Code);
        acc.Feed(Robot.ParseLine("MSG:2003,516,3,\"Error 'Scsi error - MEDIUM ERROR' occurred while reading\",\"x\",\"a\",\"b\",\"c\"")!);
        Assert.Single(acc.ReadErrors);
        Assert.Equal(2003, acc.FirstError?.Code);
        var space = new RunAccumulator();
        space.Feed(Robot.ParseLine("MSG:5038,0,0,\"The total size of all output files may reach as much as 40 Gb while there are only 10 Gb free\",\"x\"")!);
        Assert.Equal(MessageCode.SpaceMakemkvWarning, space.StopReason?.Code);
        Assert.Equal("C:/Users/me/log.txt", RunAccumulator_FilePath("file:///C:/Users/me/log.txt"));
    }

    private static string? RunAccumulator_FilePath(string url)
    {
        var acc = new RunAccumulator();
        acc.Feed(new RobotEvent.Message(new RobotMessage(1004, 0, 1, "x", "%1", new List<string> { url })));
        return acc.DebugLog;
    }
}
