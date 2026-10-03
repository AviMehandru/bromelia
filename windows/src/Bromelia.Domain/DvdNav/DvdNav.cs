using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Text;
using System.Text.RegularExpressions;

namespace Bromelia.Domain;

/// <summary>Finds the episodes inside a DVD "play all" title by reading the disc's own navigation: the title tables
/// in the IFO files and the jump commands of menus, buttons and title pre-commands (was DvdNavigation).</summary>
public static class DvdNav
{
    private const int Sector = 2048;
    /// <summary>IFO times count 30 fps; NTSC playback is 29.97 fps.</summary>
    private const double Ntsc = 1.001;

    // ---- bounds-checked reads (0 past the end) ----

    private static int U8(byte[] d, int o) => o >= 0 && o < d.Length ? d[o] : 0;
    private static int U16(byte[] d, int o) => (U8(d, o) << 8) | U8(d, o + 1);
    private static int U32(byte[] d, int o) => (U16(d, o) << 16) | U16(d, o + 2);
    private static byte[] Slice(byte[] d, int from) => from >= 0 && from < d.Length ? d[from..] : Array.Empty<byte>();
    private static byte[] Slice(byte[] d, int from, int len) => from >= 0 && from < d.Length ? d[from..Math.Min(d.Length, from + len)] : Array.Empty<byte>();

    private static int Bcd(int b) => (b >> 4) * 10 + (b & 0xF);

    private static double DvdTime(byte[] d, int o)
    {
        double fps = (U8(d, o + 3) >> 6) == 1 ? 25.0 : 30.0;
        return Bcd(U8(d, o)) * 3600 + Bcd(U8(d, o + 1)) * 60 + Bcd(U8(d, o + 2)) + Bcd(U8(d, o + 3) & 0x3F) / fps;
    }

    /// <summary>An 8-byte VM command: LinkPTTN, JumpVTS_PTT or JumpTT with its condition; null for any other.</summary>
    public static Jump? DecodeJump(byte[] bytes)
    {
        var c = bytes;
        if (c.Length < 8 || c[0] >> 5 != 1) return null;
        var cond = "";
        if ((c[1] & 0x70) != 0)
        {
            string[] ops = { "?", "&", "==", "!=", ">=", ">", "<=", "<" };
            var val = (c[1] & 0x80) != 0 ? U16(c, 4).ToString(CultureInfo.InvariantCulture) : $"GPRM{c[5] & 0xF}";
            cond = $"if GPRM{c[3] & 0xF} {ops[(c[1] >> 4) & 7]} {val}: ";
        }
        if ((c[0] & 0x10) == 0)
        {
            if ((c[1] & 0x0F) == 5) return new Jump.Ptt(null, U16(c, 6) & 0x3FF, cond);              // LinkPTTN
        }
        else
        {
            int sub = c[1] & 0x0F;
            if (sub == 5) return new Jump.Ptt(c[5] & 0x7F, U16(c, 2) & 0x3FF, cond);                // JumpVTS_PTT
            if (sub == 2) return new Jump.Title(c[5] & 0x7F, cond);                                  // JumpTT
        }
        return null;
    }

    private static List<byte[]> PgcCommands(byte[] pgc)
    {
        int off = U16(pgc, 0xE4);
        var list = new List<byte[]>();
        if (off == 0) return list;
        int n = U16(pgc, off) + U16(pgc, off + 2) + U16(pgc, off + 4);
        for (int k = 0; k < n; k++) list.Add(Slice(pgc, off + 8 + 8 * k, 8));
        return list;
    }

    private static List<byte[]> Pgcs(byte[] table)
    {
        var list = new List<byte[]>();
        for (int i = 0; i < U16(table, 0); i++) list.Add(Slice(table, U32(table, 8 + 8 * i + 4)));
        return list;
    }

    private static List<byte[]> MenuPgcs(byte[] ifo, int pointerOffset)
    {
        int sec = U32(ifo, pointerOffset);
        if (sec == 0) return new List<byte[]>();
        var ut = Slice(ifo, sec * Sector);
        var list = new List<byte[]>();
        for (int i = 0; i < U16(ut, 0); i++) list.AddRange(Pgcs(Slice(ut, U32(ut, 8 + 8 * i + 4))));
        return list;
    }

    private static bool IsNav(byte[] d, int o) =>
        o + 0x2D <= d.Length && U32(d, o + 0x0E) == 0x1BB && U32(d, o + 0x26) == 0x1BF && d[o + 0x2C] == 0;

    private static byte[] ReadAll(IByteSource source, IReadOnlyDictionary<string, long> files, string name) =>
        files.TryGetValue(name, out var size) ? source.Read(name, 0, (int)size) : Array.Empty<byte>();

    /// <summary>One pass over a menu VOB: the button commands of its NAV packs (each once, in order) and the sector
    /// span of each VOB/cell id.</summary>
    private sealed class MenuVob
    {
        public readonly List<byte[]> Buttons = new();
        public readonly List<CellRef> Stills = new();
    }

    private static MenuVob ScanMenuVob(IByteSource source, string name, long size)
    {
        var r = new MenuVob();
        var seen = new HashSet<string>();
        var cells = new Dictionary<(int, int), (long First, long Last)>();
        long total = size / Sector;
        for (long s = 0; s < total; s += 512)
        {
            var chunk = source.Read(name, s * Sector, (int)Math.Min(512, total - s) * Sector);
            if (chunk.Length == 0) break;
            for (int k = 0; k < chunk.Length / Sector; k++)
            {
                int o = k * Sector;
                if (!IsNav(chunk, o)) continue;
                int pci = o + 0x2D;
                int buttons = Math.Min(U8(chunk, pci + 0x60 + 16), 36);
                for (int b = 0; b < buttons; b++)
                {
                    var cmd = Slice(chunk, pci + 0x8E + 18 * b + 10, 8);
                    if (seen.Add(Convert.ToHexString(cmd))) r.Buttons.Add(cmd);
                }
                int dsi = o + 0x407;
                var key = (U16(chunk, dsi + 0x18), U8(chunk, dsi + 0x1B));
                long sector = s + k;
                cells[key] = cells.TryGetValue(key, out var v) ? (v.First, sector) : (sector, sector);
            }
        }
        // A still per cell longer than 8 sectors, in menu VOBs of 64 KiB to 512 MiB.
        if (size >= 64 * 1024 && size <= 512L * 1024 * 1024)
        {
            var bounds = cells.Values.OrderBy(v => v.First).ToList();
            for (int i = 0; i < bounds.Count; i++)
            {
                long end = i + 1 < bounds.Count ? bounds[i + 1].First : total;
                if (end - bounds[i].First > 8) r.Stills.Add(new CellRef(name, bounds[i].First, end));
            }
        }
        return r;
    }

    private static readonly Regex MenuVobName = new(@"^(VIDEO_TS|VTS_\d\d_0)\.VOB$", RegexOptions.CultureInvariant);

    /// <summary>The titles, chapter jumps and menu stills of the VIDEO_TS in <paramref name="source"/>; null when
    /// VIDEO_TS.IFO isn't a video manager.</summary>
    public static NavAnalysis? Analyse(IByteSource source)
    {
        var files = new Dictionary<string, long>();
        foreach (var f in source.Files()) files[f.Name] = f.Size;
        var vmg = ReadAll(source, files, "VIDEO_TS.IFO");
        if (vmg.Length <= 0xCC || Encoding.ASCII.GetString(vmg, 0, 12) != "DVDVIDEO-VMG") return null;
        var menus = new Dictionary<string, MenuVob>();
        foreach (var name in files.Keys.Where(n => MenuVobName.IsMatch(n)).OrderBy(n => n, StringComparer.Ordinal))
            menus[name] = ScanMenuVob(source, name, files[name]);
        List<byte[]> Buttons(string vob) => menus.TryGetValue(vob, out var m) ? m.Buttons : new List<byte[]>();

        var titleList = new List<NavTitle>();
        var jumps = new List<NavJump>();
        var seenJumps = new HashSet<(int, int)>();
        void Add(int title, int ptt, string how)
        {
            if (seenJumps.Add((title, ptt))) jumps.Add(new NavJump(title, ptt, how));
        }

        var tt = Slice(vmg, U32(vmg, 0xC4) * Sector);
        var titles = new Dictionary<int, (int Vts, int Ttn)>();
        for (int i = 0; i < U16(tt, 0); i++) titles[i + 1] = (U8(tt, 8 + 12 * i + 6), U8(tt, 8 + 12 * i + 7));
        var byVtsTtn = new Dictionary<(int, int), int>();
        foreach (var kv in titles) byVtsTtn[kv.Value] = kv.Key;

        foreach (var p in MenuPgcs(vmg, 0xC8))
            foreach (var c in PgcCommands(p))
                if (DecodeJump(c) is Jump.Title j) Add(j.Number, 1, "menu JumpTT");
        foreach (var c in Buttons("VIDEO_TS.VOB"))
            if (DecodeJump(c) is Jump.Title j) Add(j.Number, 1, "button JumpTT");

        foreach (var vtsn in titles.Values.Select(v => v.Vts).Distinct().OrderBy(v => v))
        {
            var name = string.Format(CultureInfo.InvariantCulture, "VTS_{0:00}_0.IFO", vtsn);
            if (!files.ContainsKey(name)) continue;
            var ifo = ReadAll(source, files, name);
            var pb = Slice(ifo, U32(ifo, 0xC8) * Sector);
            int nttu = U16(pb, 0);
            var offs = Enumerable.Range(0, nttu).Select(i => U32(pb, 8 + 4 * i)).ToList();
            offs.Add(U32(pb, 4) + 1);
            var titlePgcs = Pgcs(Slice(ifo, U32(ifo, 0xCC) * Sector));
            for (int ttn = 1; ttn <= nttu; ttn++)
            {
                if (!byVtsTtn.TryGetValue((vtsn, ttn), out var title)) continue;
                var chapters = new List<double>();
                var vobs = new List<int>();
                int? firstPgc = null;
                for (int j = offs[ttn - 1]; j + 4 <= offs[ttn]; j += 4)
                {
                    int pgcn = U16(pb, j), pgn = U16(pb, j + 2);
                    if (pgcn < 1 || pgcn > titlePgcs.Count) continue;
                    firstPgc ??= pgcn;
                    var pg = titlePgcs[pgcn - 1];
                    int nprog = U8(pg, 2), ncell = U8(pg, 3);
                    int pmo = U16(pg, 0xE6), cpbo = U16(pg, 0xE8), cpso = U16(pg, 0xEA);
                    var pmap = Enumerable.Range(0, nprog).Select(k => U8(pg, pmo + k)).ToList();
                    pmap.Add(ncell + 1);
                    if (pgn < 1 || pgn >= pmap.Count) continue;
                    double d = 0;
                    for (int cell = Math.Max(0, pmap[pgn - 1] - 1); cell < Math.Max(0, pmap[pgn] - 1); cell++) d += DvdTime(pg, cpbo + 24 * cell + 4);
                    chapters.Add(Ntsc * d);
                    vobs.Add(U16(pg, cpso + 4 * (pmap[pgn - 1] - 1)));
                }
                titleList.RemoveAll(t => t.Number == title);
                titleList.Add(new NavTitle(title, chapters, vobs));
                if (firstPgc is { } fp)
                    foreach (var c in PgcCommands(titlePgcs[fp - 1]))
                        if (DecodeJump(c) is Jump.Ptt { TitleNumber: null } j) Add(title, j.Chapter, $"{j.Condition}LinkPTTN {j.Chapter}");
            }
            var cmds = MenuPgcs(ifo, 0xD0).SelectMany(PgcCommands).Concat(Buttons(string.Format(CultureInfo.InvariantCulture, "VTS_{0:00}_0.VOB", vtsn)));
            foreach (var c in cmds)
                if (DecodeJump(c) is Jump.Ptt { TitleNumber: { } ttn2 } j && byVtsTtn.TryGetValue((vtsn, ttn2), out var t))
                    Add(t, j.Chapter, $"menu JumpVTS_PTT {ttn2}:{j.Chapter}");
        }
        return new NavAnalysis(titleList, jumps, menus.Values.SelectMany(m => m.Stills).ToList());
    }

    /// <summary>The episodes of title <paramref name="titleIndex"/>: an episode starts at every chapter a menu,
    /// button or pre-command jumps to, and at chapter 1. The last one ends at the next VOB boundary (or, in a single
    /// VOB, after as many chapters as the one before it). Null without at least two starts.</summary>
    public static EpisodePlan? EpisodePlan(NavAnalysis analysis, int titleIndex)
    {
        var t = analysis.Titles.FirstOrDefault(x => x.Number == titleIndex);
        var jumps = analysis.Jumps.Where(j => j.Title == titleIndex).ToList();
        if (t == null || t.Chapters.Count == 0 || jumps.Count == 0) return null;
        var targets = new Dictionary<int, string>();
        foreach (var j in jumps) targets.TryAdd(j.Chapter, j.How);
        targets.TryAdd(1, "title start");
        var ch = t.Chapters;
        var vobs = t.ChapterVobs;
        var starts = targets.Keys.Where(k => k >= 1 && k <= ch.Count).OrderBy(k => k).ToList();
        if (starts.Count < 2) return null;
        int last = starts[^1];
        int lastEnd = last;
        string endRule;
        if (vobs.Distinct().Count() > 1)
        {
            while (lastEnd < ch.Count && vobs[lastEnd] == vobs[last - 1]) lastEnd++;
            endRule = "VOB boundary";
        }
        else
        {
            lastEnd = Math.Min(ch.Count, last + (last - starts[^2]) - 1);
            endRule = "same chapter count as the previous episode";
        }
        var tail = Enumerable.Range(lastEnd + 1, Math.Max(0, ch.Count - lastEnd)).Where(n => ch[n - 1] >= 1.0).ToList();
        var real = Enumerable.Range(1, ch.Count).Where(n => !(n > lastEnd && ch[n - 1] < 1.0)).ToList();
        var chapterStarts = new List<double>();
        double acc = 0;
        foreach (var c in ch) { chapterStarts.Add(acc); acc += c; }
        var durations = new List<double>();
        for (int i = 0; i < starts.Count; i++)
        {
            var r = Range(starts, lastEnd, i);
            durations.Add(Enumerable.Range(r.First, r.Last - r.First + 1).Sum(n => ch[n - 1]));
        }
        return new EpisodePlan(titleIndex, starts, lastEnd, endRule, tail, starts.Skip(1).Concat(tail.Take(1)).ToList(),
            real.Sum(n => ch[n - 1]), chapterStarts, real.Count, durations, starts.Select(s => targets[s]).ToList());
    }

    /// <summary>Plans for every title that the menus jump into at two or more chapters, most episodes first.</summary>
    public static List<EpisodePlan> Plans(NavAnalysis analysis) =>
        analysis.Jumps.Select(j => j.Title).Distinct().Select(t => EpisodePlan(analysis, t)).OfType<EpisodePlan>()
            .OrderByDescending(p => p.Starts.Count).ThenBy(p => p.Title).ToList();

    /// <summary>The menu stills (one per VOB/cell id): input for MenuOcr.</summary>
    public static IReadOnlyList<CellRef> StillCells(NavAnalysis analysis) => analysis.Stills;

    private static ChapterRange Range(IReadOnlyList<int> starts, int lastEnd, int i)
    {
        int end = i + 1 < starts.Count ? starts[i + 1] - 1 : lastEnd;
        return new ChapterRange(starts[i], Math.Max(starts[i], end));
    }

    /// <summary>The disc chapters of episode <paramref name="episode"/> (0-based).</summary>
    public static ChapterRange ChapterRange(EpisodePlan plan, int episode) => Range(plan.Starts, plan.LastEnd, episode);

    /// <summary>Whether the episodes look like real episodes rather than a movie's scene selection: strict, at
    /// least 3 of 15 minutes or more within 35 % of each other; otherwise at least 2 of 5 minutes or more within a
    /// factor of 2.</summary>
    public static bool IsPlausible(EpisodePlan plan, bool strict)
    {
        if (plan.Starts.Count < (strict ? 3 : 2) || plan.EpisodeDurations.Count == 0) return false;
        double lo = plan.EpisodeDurations.Min(), hi = plan.EpisodeDurations.Max();
        if (lo <= 0) return false;
        return strict ? lo >= 900 && hi / lo <= 1.35 : lo >= 300 && hi / lo <= 2.0;
    }

    /// <summary>Whether a ripped title with <paramref name="chapters"/> chapters is this plan's title (MakeMKV may
    /// drop one short chapter).</summary>
    public static bool MatchesChapterCount(EpisodePlan plan, int chapters) => chapters >= plan.KeptChapters - 1 && chapters <= plan.ChapterStarts.Count;

    /// <summary>The MKV chapters to split at for the disc chapters <paramref name="split"/>, by the MKV's chapter
    /// starts (which may begin with an added chapter 00); the disc's numbers when the starts are unknown; null when
    /// a chapter has no MKV chapter within 1 s.</summary>
    public static List<int>? MkvChapters(IReadOnlyList<int> split, EpisodePlan plan, IReadOnlyList<double>? mkvStarts)
    {
        if (mkvStarts == null || mkvStarts.Count == 0) return split.ToList();
        var result = new List<int>();
        foreach (var c in split)
        {
            double want = plan.ChapterStarts[c - 1];
            int best = 0;
            for (int i = 1; i < mkvStarts.Count; i++)
                if (Math.Abs(mkvStarts[i] - want) < Math.Abs(mkvStarts[best] - want)) best = i;
            if (Math.Abs(mkvStarts[best] - want) > 1.0) return null;
            result.Add(best + 1);
        }
        return result;
    }
}
