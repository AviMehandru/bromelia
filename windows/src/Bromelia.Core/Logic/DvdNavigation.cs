using System.Globalization;
using System.Text;
using System.Text.RegularExpressions;

namespace Bromelia.Core.Logic;

// Finds the episodes inside a DVD "play all" title by reading the disc's own navigation: the title tables in
// the IFO files and the jump commands of menus, buttons and title pre-commands. Works on an ISO image, a
// VIDEO_TS folder (backup) or a mounted disc. Same analysis as the macOS and Linux versions.

/// <summary>Read access to the files of a VIDEO_TS directory.</summary>
public interface IVideoTsReader : IDisposable
{
    string Label { get; }
    /// <summary>Upper-case file name → size in bytes.</summary>
    IReadOnlyDictionary<string, long> Files { get; }
    /// <summary>Reads <paramref name="count"/> 2048-byte sectors from <paramref name="sector"/>; short reads return fewer bytes.</summary>
    byte[] Read(string file, long sector, int count);
}

public static class VideoTsReaderExtensions
{
    public static byte[] ReadAll(this IVideoTsReader r, string file)
    {
        if (!r.Files.TryGetValue(file, out var size)) return Array.Empty<byte>();
        var d = r.Read(file, 0, (int)((size + DvdNavigation.Sector - 1) / DvdNavigation.Sector));
        return d.Length > size ? d[..(int)size] : d;
    }
}

/// <summary>VIDEO_TS inside an ISO 9660 / UDF bridge image.</summary>
public sealed class IsoVideoTs : IVideoTsReader
{
    readonly FileStream _fs;
    readonly Dictionary<string, long> _files = new();
    readonly Dictionary<string, long> _lba = new();

    public string Label { get; }
    public IReadOnlyDictionary<string, long> Files => _files;

    IsoVideoTs(FileStream fs, string label) { _fs = fs; Label = label; }

    public static IsoVideoTs? Open(string path)
    {
        FileStream fs;
        try { fs = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return null; }
        var pvd = ReadSectors(fs, 16, 1);
        if (pvd.Length < 190 || Encoding.ASCII.GetString(pvd, 1, 5) != "CD001") { fs.Dispose(); return null; }
        var r = new IsoVideoTs(fs, Encoding.ASCII.GetString(pvd, 40, 32).Trim(' ', '\0'));
        var root = pvd.AsSpan(156, 34).ToArray();
        var vts = Directory(fs, B.U32Le(root, 2), B.U32Le(root, 10)).FirstOrDefault(e => e.Name.ToUpperInvariant() == "VIDEO_TS");
        if (vts.Name == null) { fs.Dispose(); return null; }
        foreach (var e in Directory(fs, vts.Lba, vts.Size))
        {
            r._files[e.Name.ToUpperInvariant()] = e.Size;
            r._lba[e.Name.ToUpperInvariant()] = e.Lba;
        }
        return r;
    }

    public byte[] Read(string file, long sector, int count) =>
        _lba.TryGetValue(file, out var start) ? ReadSectors(_fs, start + sector, count) : Array.Empty<byte>();

    static byte[] ReadSectors(FileStream fs, long lba, int count)
    {
        try
        {
            fs.Seek(lba * DvdNavigation.Sector, SeekOrigin.Begin);
            var buf = new byte[count * DvdNavigation.Sector];
            int total = 0, n;
            while (total < buf.Length && (n = fs.Read(buf, total, buf.Length - total)) > 0) total += n;
            return total == buf.Length ? buf : buf[..total];
        }
        catch (IOException) { return Array.Empty<byte>(); }
    }

    static List<(string Name, long Lba, long Size)> Directory(FileStream fs, long lba, long size)
    {
        var data = ReadSectors(fs, lba, (int)((size + DvdNavigation.Sector - 1) / DvdNavigation.Sector));
        if (data.Length > size) data = data[..(int)size];
        var outList = new List<(string, long, long)>();
        int i = 0;
        while (i < data.Length)
        {
            int len = data[i];
            if (len == 0) { i = (i / DvdNavigation.Sector + 1) * DvdNavigation.Sector; continue; }
            if (i + len > data.Length || len < 34) break;
            int nameLen = data[i + 32];
            if (33 + nameLen <= len)
            {
                var name = Encoding.ASCII.GetString(data, i + 33, nameLen).Split(';')[0];
                if (name != "\0" && name != "\u0001") outList.Add((name, B.U32Le(data, i + 2), B.U32Le(data, i + 10)));
            }
            i += len;
        }
        return outList;
    }

    public void Dispose() => _fs.Dispose();
}

/// <summary>A VIDEO_TS folder on disk (a backup, or a mounted disc).</summary>
public sealed class FolderVideoTs : IVideoTsReader
{
    readonly string _dir;
    readonly Dictionary<string, long> _files = new();
    readonly Dictionary<string, string> _names = new();

    public string Label { get; }
    public IReadOnlyDictionary<string, long> Files => _files;

    FolderVideoTs(string dir, string label) { _dir = dir; Label = label; }

    /// <summary><paramref name="path"/> may be the VIDEO_TS folder itself or the folder containing it.</summary>
    public static FolderVideoTs? Open(string path, string label = "")
    {
        try
        {
            var d = path.TrimEnd('/', '\\');
            if (!System.IO.Directory.Exists(d)) return null;
            if (!string.Equals(Path.GetFileName(d), "VIDEO_TS", StringComparison.OrdinalIgnoreCase))
            {
                var sub = System.IO.Directory.EnumerateDirectories(d).FirstOrDefault(x => string.Equals(Path.GetFileName(x), "VIDEO_TS", StringComparison.OrdinalIgnoreCase));
                if (sub == null) return null;
                d = sub;
            }
            var r = new FolderVideoTs(d, label.Length > 0 ? label : Path.GetFileName(Path.GetDirectoryName(d)) ?? "");
            foreach (var f in System.IO.Directory.EnumerateFiles(d))
            {
                var n = Path.GetFileName(f).ToUpperInvariant();
                r._files[n] = new FileInfo(f).Length;
                r._names[n] = f;
            }
            return r._files.ContainsKey("VIDEO_TS.IFO") ? r : null;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return null; }
    }

    public byte[] Read(string file, long sector, int count)
    {
        if (!_names.TryGetValue(file, out var path)) return Array.Empty<byte>();
        try
        {
            using var fs = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read);
            fs.Seek(sector * DvdNavigation.Sector, SeekOrigin.Begin);
            var buf = new byte[count * DvdNavigation.Sector];
            int total = 0, n;
            while (total < buf.Length && (n = fs.Read(buf, total, buf.Length - total)) > 0) total += n;
            return total == buf.Length ? buf : buf[..total];
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return Array.Empty<byte>(); }
    }

    public void Dispose() { }
}

/// <summary>Bounds-checked big / little endian reads (0 past the end).</summary>
static class B
{
    public static int U8(byte[] d, int o) => o >= 0 && o < d.Length ? d[o] : 0;
    public static int U16(byte[] d, int o) => (U8(d, o) << 8) | U8(d, o + 1);
    public static int U32(byte[] d, int o) => (U16(d, o) << 16) | U16(d, o + 2);
    public static long U32Le(byte[] d, int o) => (uint)(U8(d, o) | U8(d, o + 1) << 8 | U8(d, o + 2) << 16 | U8(d, o + 3) << 24);
    public static byte[] Slice(byte[] d, int from) => from >= 0 && from < d.Length ? d[from..] : Array.Empty<byte>();
    public static byte[] Slice(byte[] d, int from, int len) =>
        from >= 0 && from < d.Length ? d[from..Math.Min(d.Length, from + len)] : Array.Empty<byte>();
}

public static class DvdNavigation
{
    public const int Sector = 2048;
    /// <summary>IFO times count 30 fps; NTSC playback is 29.97 fps.</summary>
    public const double Ntsc = 1.001;

    public sealed record Title(List<double> Chapters, List<int> ChapterVobs);

    public sealed class Analysis
    {
        public string Label { get; init; } = "";
        public Dictionary<int, Title> Titles { get; } = new();
        /// <summary>Title number → chapter → how it is reached.</summary>
        public Dictionary<int, Dictionary<int, string>> Jumps { get; } = new();
    }

    /// <summary>Where the episodes of one title start and end.</summary>
    public sealed class EpisodePlan
    {
        public int Title { get; init; }
        /// <summary>First chapter (1-based) of each episode.</summary>
        public List<int> Starts { get; init; } = new();
        public int LastEnd { get; init; }
        public string EndRule { get; init; } = "";
        /// <summary>Chapters (≥ 1 s) played after the last episode.</summary>
        public List<int> Tail { get; init; } = new();
        public List<double> ChapterStarts { get; init; } = new();
        public double Duration { get; init; }
        public int KeptChapters { get; init; }
        public List<double> EpisodeDurations { get; set; } = new();
        public List<string> Reasons { get; init; } = new();

        public int EpisodeCount => Starts.Count;
        public List<int> SplitChapters => Starts.Skip(1).Concat(Tail.Take(1)).ToList();

        public (int First, int Last) ChapterRange(int i)
        {
            int end = i + 1 < Starts.Count ? Starts[i + 1] - 1 : LastEnd;
            return (Starts[i], Math.Max(Starts[i], end));
        }

        public bool MatchesChapterCount(int chapters) => chapters >= KeptChapters - 1 && chapters <= ChapterStarts.Count;

        /// <summary>Whether the episodes look like real episodes rather than a movie's scene selection.</summary>
        public bool IsPlausible(bool strict)
        {
            if (EpisodeCount < (strict ? 3 : 2) || EpisodeDurations.Count == 0) return false;
            double lo = EpisodeDurations.Min(), hi = EpisodeDurations.Max();
            if (lo <= 0) return false;
            return strict ? lo >= 900 && hi / lo <= 1.35 : lo >= 300 && hi / lo <= 2.0;
        }
    }

    // --- IFO parsing ---------------------------------------------------------------------------------

    static int Bcd(int b) => (b >> 4) * 10 + (b & 0xF);

    static double DvdTime(byte[] d, int o)
    {
        double fps = (B.U8(d, o + 3) >> 6) == 1 ? 25.0 : 30.0;
        return Bcd(B.U8(d, o)) * 3600 + Bcd(B.U8(d, o + 1)) * 60 + Bcd(B.U8(d, o + 2)) + Bcd(B.U8(d, o + 3) & 0x3F) / fps;
    }

    /// <summary>A decoded jump: to a chapter (TitleNumber = VTS title number, null = current title) or to a disc title.</summary>
    public sealed record Jump(bool IsChapter, int? TitleNumber, int Target, string Condition);

    public static Jump? DecodeJump(byte[] c)
    {
        if (c.Length < 8 || c[0] >> 5 != 1) return null;
        var cond = "";
        if ((c[1] & 0x70) != 0)
        {
            string[] ops = { "?", "&", "==", "!=", ">=", ">", "<=", "<" };
            var val = (c[1] & 0x80) != 0 ? B.U16(c, 4).ToString(CultureInfo.InvariantCulture) : $"GPRM{c[5] & 0xF}";
            cond = $"if GPRM{c[3] & 0xF} {ops[(c[1] >> 4) & 7]} {val}: ";
        }
        if ((c[0] & 0x10) == 0)
        {
            if ((c[1] & 0x0F) == 5) return new Jump(true, null, B.U16(c, 6) & 0x3FF, cond);          // LinkPTTN
        }
        else
        {
            int sub = c[1] & 0x0F;
            if (sub == 5) return new Jump(true, c[5] & 0x7F, B.U16(c, 2) & 0x3FF, cond);              // JumpVTS_PTT
            if (sub == 2) return new Jump(false, null, c[5] & 0x7F, cond);                              // JumpTT
        }
        return null;
    }

    static List<byte[]> PgcCommands(byte[] pgc)
    {
        int off = B.U16(pgc, 0xE4);
        var list = new List<byte[]>();
        if (off == 0) return list;
        int n = B.U16(pgc, off) + B.U16(pgc, off + 2) + B.U16(pgc, off + 4);
        for (int k = 0; k < n; k++) list.Add(B.Slice(pgc, off + 8 + 8 * k, 8));
        return list;
    }

    static List<byte[]> Pgcs(byte[] table)
    {
        var list = new List<byte[]>();
        for (int i = 0; i < B.U16(table, 0); i++) list.Add(B.Slice(table, B.U32(table, 8 + 8 * i + 4)));
        return list;
    }

    static List<byte[]> MenuPgcs(byte[] ifo, int pointerOffset)
    {
        int sec = B.U32(ifo, pointerOffset);
        if (sec == 0) return new List<byte[]>();
        var ut = B.Slice(ifo, sec * Sector);
        var list = new List<byte[]>();
        for (int i = 0; i < B.U16(ut, 0); i++) list.AddRange(Pgcs(B.Slice(ut, B.U32(ut, 8 + 8 * i + 4))));
        return list;
    }

    static bool IsNav(byte[] d, int o) =>
        o + 0x2D <= d.Length && B.U32(d, o + 0x0E) == 0x1BB && B.U32(d, o + 0x26) == 0x1BF && d[o + 0x2C] == 0;

    static List<byte[]> ButtonCommands(IVideoTsReader r, string vob)
    {
        var list = new List<byte[]>();
        if (!r.Files.TryGetValue(vob, out var size)) return list;
        var seen = new HashSet<string>();
        long total = size / Sector;
        for (long s = 0; s < total; s += 512)
        {
            var chunk = r.Read(vob, s, (int)Math.Min(512, total - s));
            if (chunk.Length == 0) break;
            for (int k = 0; k < chunk.Length / Sector; k++)
            {
                int o = k * Sector;
                if (!IsNav(chunk, o)) continue;
                int pci = o + 0x2D;
                int buttons = Math.Min(B.U8(chunk, pci + 0x60 + 16), 36);
                for (int b = 0; b < buttons; b++)
                {
                    var cmd = B.Slice(chunk, pci + 0x8E + 18 * b + 10, 8);
                    if (seen.Add(Convert.ToHexString(cmd))) list.Add(cmd);
                }
            }
        }
        return list;
    }

    public static Analysis? Analyse(IVideoTsReader r)
    {
        var vmg = r.ReadAll("VIDEO_TS.IFO");
        if (vmg.Length <= 0xCC || Encoding.ASCII.GetString(vmg, 0, 12) != "DVDVIDEO-VMG") return null;
        var a = new Analysis { Label = r.Label };
        var tt = B.Slice(vmg, B.U32(vmg, 0xC4) * Sector);
        var titles = new Dictionary<int, (int Vts, int Ttn)>();
        for (int i = 0; i < B.U16(tt, 0); i++) titles[i + 1] = (B.U8(tt, 8 + 12 * i + 6), B.U8(tt, 8 + 12 * i + 7));
        var byVtsTtn = new Dictionary<(int, int), int>();
        foreach (var kv in titles) byVtsTtn[kv.Value] = kv.Key;
        void Add(int title, int ptt, string how)
        {
            if (!a.Jumps.TryGetValue(title, out var m)) a.Jumps[title] = m = new Dictionary<int, string>();
            m.TryAdd(ptt, how);
        }

        foreach (var p in MenuPgcs(vmg, 0xC8))
            foreach (var c in PgcCommands(p))
                if (DecodeJump(c) is { IsChapter: false } j) Add(j.Target, 1, "menu JumpTT");
        foreach (var c in ButtonCommands(r, "VIDEO_TS.VOB"))
            if (DecodeJump(c) is { IsChapter: false } j) Add(j.Target, 1, "button JumpTT");

        foreach (var vtsn in titles.Values.Select(v => v.Vts).Distinct().OrderBy(v => v))
        {
            var name = $"VTS_{vtsn:00}_0.IFO";
            if (!r.Files.ContainsKey(name)) continue;
            var ifo = r.ReadAll(name);
            var pb = B.Slice(ifo, B.U32(ifo, 0xC8) * Sector);
            int nttu = B.U16(pb, 0);
            var offs = Enumerable.Range(0, nttu).Select(i => B.U32(pb, 8 + 4 * i)).ToList();
            offs.Add(B.U32(pb, 4) + 1);
            var titlePgcs = Pgcs(B.Slice(ifo, B.U32(ifo, 0xCC) * Sector));
            for (int ttn = 1; ttn <= nttu; ttn++)
            {
                if (!byVtsTtn.TryGetValue((vtsn, ttn), out var title)) continue;
                var chapters = new List<double>();
                var vobs = new List<int>();
                int? firstPgc = null;
                for (int j = offs[ttn - 1]; j + 4 <= offs[ttn]; j += 4)
                {
                    int pgcn = B.U16(pb, j), pgn = B.U16(pb, j + 2);
                    if (pgcn < 1 || pgcn > titlePgcs.Count) continue;
                    firstPgc ??= pgcn;
                    var pg = titlePgcs[pgcn - 1];
                    int nprog = B.U8(pg, 2), ncell = B.U8(pg, 3);
                    int pmo = B.U16(pg, 0xE6), cpbo = B.U16(pg, 0xE8), cpso = B.U16(pg, 0xEA);
                    var pmap = Enumerable.Range(0, nprog).Select(k => B.U8(pg, pmo + k)).ToList();
                    pmap.Add(ncell + 1);
                    if (pgn < 1 || pgn >= pmap.Count) continue;
                    double d = 0;
                    for (int cell = Math.Max(0, pmap[pgn - 1] - 1); cell < Math.Max(0, pmap[pgn] - 1); cell++) d += DvdTime(pg, cpbo + 24 * cell + 4);
                    chapters.Add(Ntsc * d);
                    vobs.Add(B.U16(pg, cpso + 4 * (pmap[pgn - 1] - 1)));
                }
                a.Titles[title] = new Title(chapters, vobs);
                if (firstPgc is { } fp)
                    foreach (var c in PgcCommands(titlePgcs[fp - 1]))
                        if (DecodeJump(c) is { IsChapter: true, TitleNumber: null } j) Add(title, j.Target, $"{j.Condition}LinkPTTN {j.Target}");
            }
            var cmds = MenuPgcs(ifo, 0xD0).SelectMany(PgcCommands).Concat(ButtonCommands(r, $"VTS_{vtsn:00}_0.VOB"));
            foreach (var c in cmds)
                if (DecodeJump(c) is { IsChapter: true, TitleNumber: { } ttn2 } j && byVtsTtn.TryGetValue((vtsn, ttn2), out var t))
                    Add(t, j.Target, $"menu JumpVTS_PTT {ttn2}:{j.Target}");
        }
        return a;
    }

    // --- Episode plan --------------------------------------------------------------------------------

    public static EpisodePlan? Plan(Analysis a, int title)
    {
        if (!a.Titles.TryGetValue(title, out var t) || t.Chapters.Count == 0 || !a.Jumps.TryGetValue(title, out var jumps)) return null;
        var targets = new Dictionary<int, string>(jumps);
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
        var plan = new EpisodePlan
        {
            Title = title, Starts = starts, LastEnd = lastEnd, EndRule = endRule, Tail = tail, ChapterStarts = chapterStarts,
            Duration = real.Sum(n => ch[n - 1]), KeptChapters = real.Count, Reasons = starts.Select(s => targets[s]).ToList(),
        };
        plan.EpisodeDurations = Enumerable.Range(0, starts.Count).Select(i =>
        {
            var (f, l) = plan.ChapterRange(i);
            return Enumerable.Range(f, l - f + 1).Sum(n => ch[n - 1]);
        }).ToList();
        return plan;
    }

    /// <summary>Plans for every title that the menus jump into at two or more chapters, most episodes first.</summary>
    public static List<EpisodePlan> Plans(Analysis a) =>
        a.Jumps.Keys.Select(t => Plan(a, t)).OfType<EpisodePlan>().OrderByDescending(p => p.EpisodeCount).ThenBy(p => p.Title).ToList();

    // --- Episode numbers from the menus --------------------------------------------------------------

    /// <summary>Raw MPEG-PS data of every menu still (one per VOB/cell id) — input for OCR.</summary>
    public static List<byte[]> MenuStills(IVideoTsReader r)
    {
        var list = new List<byte[]>();
        foreach (var name in r.Files.Keys.OrderBy(k => k, StringComparer.Ordinal))
        {
            if (name != "VIDEO_TS.VOB" && !Regex.IsMatch(name, @"^VTS_\d\d_0\.VOB$")) continue;
            var size = r.Files[name];
            if (size < 64 * 1024 || size > 512L * 1024 * 1024) continue;
            var data = r.ReadAll(name);
            int total = data.Length / Sector;
            var cells = new Dictionary<(int, int), (int First, int Last)>();
            for (int s = 0; s < total; s++)
            {
                if (!IsNav(data, s * Sector)) continue;
                int dsi = s * Sector + 0x407;
                var key = (B.U16(data, dsi + 0x18), B.U8(data, dsi + 0x1B));
                cells[key] = cells.TryGetValue(key, out var v) ? (v.First, s) : (s, s);
            }
            var bounds = cells.Values.OrderBy(v => v.First).ToList();
            for (int i = 0; i < bounds.Count; i++)
            {
                int end = i + 1 < bounds.Count ? bounds[i + 1].First : total;
                if (end - bounds[i].First > 8) list.Add(data[(bounds[i].First * Sector)..(end * Sector)]);
            }
        }
        return list;
    }

    static readonly Regex EpisodeText = new(@"EPIS[O0]DE\s*#?\s*(\d{1,4})\b", RegexOptions.IgnoreCase | RegexOptions.CultureInvariant);

    public static HashSet<int> EpisodeNumbersInText(string text) =>
        EpisodeText.Matches(text).Select(m => int.Parse(m.Groups[1].Value, CultureInfo.InvariantCulture)).ToHashSet();

    /// <summary>The first episode S such that [S, S+count) covers the most numbers; null without a unique
    /// best choice backed by at least two numbers.</summary>
    public static int? FirstEpisode(ISet<int> numbers, int count)
    {
        if (numbers.Count == 0 || count <= 0) return null;
        var scores = new Dictionary<int, int>();
        foreach (var n in numbers)
            for (int k = 0; k < count; k++)
                if (n - k >= 0) scores[n - k] = 0;
        foreach (var s in scores.Keys.ToList()) scores[s] = numbers.Count(n => s <= n && n < s + count);
        int best = scores.Values.Max();
        var winners = scores.Where(kv => kv.Value == best).Select(kv => kv.Key).ToList();
        return winners.Count == 1 && best >= 2 ? winners[0] : null;
    }

    /// <summary>Maps disc chapter numbers to MKV chapter numbers using the MKV's chapter start times when known.</summary>
    public static List<int>? MkvChapters(IReadOnlyList<int> discChapters, EpisodePlan plan, IReadOnlyList<double>? mkvStarts)
    {
        if (mkvStarts == null || mkvStarts.Count == 0) return discChapters.ToList();
        var result = new List<int>();
        foreach (var c in discChapters)
        {
            double want = plan.ChapterStarts[c - 1];
            int best = Enumerable.Range(0, mkvStarts.Count).MinBy(i => Math.Abs(mkvStarts[i] - want));
            if (Math.Abs(mkvStarts[best] - want) > 1.0) return null;
            result.Add(best + 1);
        }
        return result;
    }

    public static string Hms(double s) =>
        string.Format(CultureInfo.InvariantCulture, "{0}:{1:00}:{2:00.000}", (int)s / 3600, (int)s % 3600 / 60, s % 60);
}
