using System.Globalization;
using Bromelia.Domain;
using Bromelia.Foundation;
using Xunit;
using static Bromelia.Tests.Fixtures;

namespace Bromelia.Domain.Tests;

/// <summary>A VIDEO_TS folder of the fixtures as a ByteSource (the adapter comes in phase 2).</summary>
internal sealed class FolderByteSource : IByteSource
{
    private readonly string _dir;

    public FolderByteSource(string dir) { _dir = dir; }

    public byte[] Read(string path, long offset, int length)
    {
        var file = Directory.EnumerateFiles(_dir).FirstOrDefault(f => Path.GetFileName(f).ToUpperInvariant() == path);
        if (file == null) return Array.Empty<byte>();
        var all = File.ReadAllBytes(file);
        if (offset >= all.Length) return Array.Empty<byte>();
        return all[(int)offset..(int)Math.Min(all.Length, offset + length)];
    }

    public IReadOnlyList<ByteFile> Files() => Directory.EnumerateFiles(_dir)
        .Select(f => new ByteFile(Path.GetFileName(f).ToUpperInvariant(), new FileInfo(f).Length)).OrderBy(f => f.Name, StringComparer.Ordinal).ToList();
}

/// <summary>Files held in memory.</summary>
internal sealed class MemoryByteSource : IByteSource
{
    private readonly Dictionary<string, byte[]> _files;

    public MemoryByteSource(Dictionary<string, byte[]> files) { _files = files; }

    public byte[] Read(string path, long offset, int length) =>
        _files.TryGetValue(path, out var d) && offset < d.Length ? d[(int)offset..(int)Math.Min(d.Length, offset + length)] : Array.Empty<byte>();

    public IReadOnlyList<ByteFile> Files() => _files.Select(f => new ByteFile(f.Key, f.Value.Length)).ToList();
}

public class DvdNavTests
{
    /// <summary>A menu VOB of 48 sectors: NAV packs at sectors 0 (cell 1), 20 and 30 (cell 2); the first has a
    /// button that jumps to title 4.</summary>
    internal static Dictionary<string, byte[]> MenuDisc()
    {
        var vmg = new byte[0xD0];
        System.Text.Encoding.ASCII.GetBytes("DVDVIDEO-VMG").CopyTo(vmg, 0);
        var vob = new byte[48 * 2048];
        void Nav(int sector, int cell, bool button)
        {
            int o = sector * 2048;
            vob[o + 0x10] = 1; vob[o + 0x11] = 0xBB;
            vob[o + 0x28] = 1; vob[o + 0x29] = 0xBF;
            int dsi = o + 0x407;
            vob[dsi + 0x19] = 1;
            vob[dsi + 0x1B] = (byte)cell;
            if (!button) return;
            int pci = o + 0x2D;
            vob[pci + 0x60 + 16] = 1;
            new byte[] { 48, 2, 0, 0, 0, 4, 0, 0 }.CopyTo(vob, pci + 0x8E + 10);
        }
        Nav(0, 1, true);
        Nav(20, 2, false);
        Nav(30, 2, false);
        return new Dictionary<string, byte[]> { ["VIDEO_TS.IFO"] = vmg, ["VIDEO_TS.VOB"] = vob };
    }

    [Fact]
    public void MenuStillsAndButtons()
    {
        var a = DvdNav.Analyse(new MemoryByteSource(MenuDisc()))!;
        Assert.Equal(new[] { new CellRef("VIDEO_TS.VOB", 0, 20), new CellRef("VIDEO_TS.VOB", 20, 48) }, DvdNav.StillCells(a));
        Assert.Equal(new[] { new NavJump(4, 1, "button JumpTT") }, a.Jumps);
        Assert.Empty(a.Titles);
        Assert.Null(DvdNav.Analyse(new MemoryByteSource(new Dictionary<string, byte[]> { ["VIDEO_TS.IFO"] = new byte[0x100] })));
    }

    /// <summary>h:mm:ss.fff, as the fixtures write durations.</summary>
    internal static string Hms(double s) =>
        string.Format(CultureInfo.InvariantCulture, "{0}:{1:00}:{2:00.000}", (int)s / 3600, (int)s % 3600 / 60, s % 60);

    internal static EpisodePlan FirstPlan(string golden)
    {
        var g = Json("domain/" + golden);
        var analysis = DvdNav.Analyse(new FolderByteSource(Path_(g["input"]!.AsString!)))!;
        return DvdNav.Plans(analysis)[0];
    }

    private static JsonValue Ints(IEnumerable<int> v) => JsonValue.Of(v.Select(i => JsonValue.Of(i)));

    [Fact]
    public void FindsPlayAllEpisodes()
    {
        var want = Json("domain/dvd-play-all.navigation.expected.json")["expect"]!["plan"]!;
        var plan = FirstPlan("dvd-play-all.navigation.expected.json");
        Same(want["title"]!.AsInteger, (long)plan.Title, "title");
        Same(want["starts"], Ints(plan.Starts), "starts");
        Same(want["lastEnd"]!.AsInteger, (long)plan.LastEnd, "lastEnd");
        Same(want["endRule"]!.AsString, plan.EndRule, "endRule");
        Same(want["tail"], Ints(plan.Tail), "tail");
        Same(want["splitChapters"], Ints(plan.SplitChapters), "splitChapters");
        Same(want["duration"]!.AsString, Hms(plan.Duration), "duration");
        Same(want["chapterStart8"]!.AsString, Hms(plan.ChapterStarts[7]), "chapterStart8");
        foreach (var m in want["chapterRanges"]!.AsObject!)
        {
            var r = DvdNav.ChapterRange(plan, int.Parse(m.Key, CultureInfo.InvariantCulture));
            Same(m.Value, Ints(new[] { r.First, r.Last }), "chapterRange " + m.Key);
        }
        Same(want["plausibleStrict"]!.AsBool, DvdNav.IsPlausible(plan, true), "plausibleStrict");
        foreach (var m in want["reasons"]!.AsObject!)
            Same(m.Value.AsString, plan.Reasons[int.Parse(m.Key, CultureInfo.InvariantCulture)], "reason " + m.Key);
        Assert.True(DvdNav.MatchesChapterCount(plan, plan.KeptChapters));
        Assert.False(DvdNav.MatchesChapterCount(plan, plan.KeptChapters - 2));
    }

    [Fact]
    public void NavigationCases() => RunCases("domain/dvdnav.cases.json", (id, given, expect) =>
    {
        if (given["bytes"] is { } bytes)
        {
            var jump = DvdNav.DecodeJump(bytes.AsArray!.Select(b => (byte)b.AsInteger!).ToArray());
            JsonValue got = jump switch
            {
                Jump.Ptt p => JsonValue.Of(("kind", JsonValue.Of("ptt")), ("titleNumber", p.TitleNumber is { } n ? JsonValue.Of(n) : JsonValue.Null.Instance),
                    ("ptt", JsonValue.Of(p.Chapter)), ("condition", JsonValue.Of(p.Condition))),
                Jump.Title t => JsonValue.Of(("kind", JsonValue.Of("title")), ("title", JsonValue.Of(t.Number)), ("condition", JsonValue.Of(t.Condition))),
                _ => JsonValue.Null.Instance,
            };
            Same(expect["jump"], got, "jump");
            return true;
        }
        var plan = FirstPlan(given["plan"]!.AsString!);
        var split = given["split"]!.AsArray!.Select(c => (int)c.AsInteger!).ToList();
        List<double>? starts = given["mkvStarts"] switch
        {
            // "a chapter 00 at 0, then every disc chapter start + 0.02 s"
            JsonValue.String => new[] { 0.0 }.Concat(plan.ChapterStarts.Select(s => s + 0.02)).ToList(),
            JsonValue.Array a => a.Items.Select(x => x.AsNumber!.Value).ToList(),
            _ => null,
        };
        var chapters = DvdNav.MkvChapters(split, plan, starts);
        Same(expect["chapters"], chapters == null ? JsonValue.Null.Instance : Ints(chapters), "chapters");
        return true;
    });
}
