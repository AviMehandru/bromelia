using System.Globalization;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;
using Bromelia.Core.Config;
using Bromelia.Core.Logic;
using Bromelia.Core.Robot;

namespace Bromelia.Core.Engine;

/// <summary>SHA-256 checksums of produced files, written as SHA256SUMS in the output folder in the format of
/// sha256sum / shasum -a 256 (check later with <c>sha256sum -c SHA256SUMS</c>).</summary>
public static class Checksums
{
    public const string FileName = "SHA256SUMS";

    public sealed record Entry(string Path, long Size, string Sha256);

    /// <summary>Every regular file under <paramref name="items"/> (folders walked recursively), relative to
    /// <paramref name="baseDir"/> with '/' separators, sorted.</summary>
    public static List<(string Full, string Relative)> Files(IEnumerable<string> items, string baseDir)
    {
        var basePath = System.IO.Path.GetFullPath(baseDir).TrimEnd(System.IO.Path.DirectorySeparatorChar, '/');
        string Rel(string p)
        {
            var full = System.IO.Path.GetFullPath(p);
            var r = full.StartsWith(basePath + System.IO.Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase)
                ? full[(basePath.Length + 1)..] : System.IO.Path.GetFileName(full);
            return r.Replace('\\', '/');
        }
        var list = new List<(string, string)>();
        foreach (var item in items)
        {
            if (Directory.Exists(item))
            {
                foreach (var f in Directory.EnumerateFiles(item, "*", SearchOption.AllDirectories))
                    if (!System.IO.Path.GetFileName(f).StartsWith('.')) list.Add((f, Rel(f)));
            }
            else if (File.Exists(item)) list.Add((item, Rel(item)));
        }
        return list.GroupBy(x => x.Item2).Select(g => g.First()).OrderBy(x => x.Item2, StringComparer.Ordinal).ToList();
    }

    /// <summary>Streams a file through SHA-256; <paramref name="progress"/> receives the bytes read so far.</summary>
    public static string Sha256(string path, Action<long>? progress = null, CancellationToken ct = default)
    {
        using var fs = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read, 1 << 20);
        using var h = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        var buf = new byte[8 << 20];
        long done = 0;
        int n;
        while ((n = fs.Read(buf, 0, buf.Length)) > 0)
        {
            ct.ThrowIfCancellationRequested();
            h.AppendData(buf, 0, n);
            done += n;
            progress?.Invoke(done);
        }
        return Convert.ToHexString(h.GetHashAndReset()).ToLowerInvariant();
    }

    public static string Render(IEnumerable<(string Path, string Hash)> entries) =>
        string.Concat(entries.Select(e => $"{e.Hash}  {e.Path}\n"));

    static readonly Regex Line = new(@"^([0-9a-fA-F]{64}) [ *](.+)$");

    /// <summary>Parses a SHA256SUMS file ("hash  path" or "hash *path").</summary>
    public static List<(string Hash, string Path)> Parse(string text) =>
        text.Split('\n').Select(l => Line.Match(l.TrimEnd('\r'))).Where(m => m.Success)
            .Select(m => (m.Groups[1].Value.ToLowerInvariant(), m.Groups[2].Value)).ToList();

    /// <summary>Re-hashes the files listed in folder/SHA256SUMS; returns those that are missing or differ.</summary>
    public static List<string> Verify(string folder, CancellationToken ct = default)
    {
        var bad = new List<string>();
        foreach (var (hash, path) in Parse(File.ReadAllText(System.IO.Path.Combine(folder, FileName))))
        {
            var full = System.IO.Path.Combine(folder, path.Replace('/', System.IO.Path.DirectorySeparatorChar));
            try { if (!File.Exists(full) || Sha256(full, null, ct) != hash) bad.Add(path); }
            catch (IOException) { bad.Add(path); }
        }
        return bad;
    }

    /// <summary>Writes SHA256SUMS, keeping entries of earlier jobs that wrote to the same folder.</summary>
    public static string WriteMerged(string folder, IEnumerable<Entry> entries)
    {
        var path = System.IO.Path.Combine(folder, FileName);
        var merged = new SortedDictionary<string, string>(StringComparer.Ordinal);
        if (File.Exists(path)) foreach (var (h, p) in Parse(File.ReadAllText(path))) merged[p] = h;
        foreach (var e in entries) merged[e.Path] = e.Sha256;
        File.WriteAllText(path, Render(merged.Select(kv => (kv.Key, kv.Value))), new UTF8Encoding(false));
        return path;
    }
}

/// <summary>bromelia.json: a self-describing record of what was archived, kept next to the files.</summary>
public sealed class ArchiveRecord
{
    public sealed class DiscEntry
    {
        public string Label { get; set; } = "";
        public string VolumeName { get; set; } = "";
        public string Type { get; set; } = "";
        public string Format { get; set; } = "";
        public string FormatCode { get; set; } = "";
        public bool Encrypted { get; set; }
        public int? Season { get; set; }
        public int? Part { get; set; }
        public int? Volume { get; set; }
        public int? Disc { get; set; }
        /// <summary>The disc's fingerprint (<see cref="DiscFingerprint"/>), to recognise it when it is inserted again.</summary>
        public string? Fingerprint { get; set; }
    }

    public sealed class TitleEntry
    {
        public int Index { get; set; }
        public int? SourceTitleId { get; set; }
        public string SourceFile { get; set; } = "";
        public string Duration { get; set; } = "";
        public int Chapters { get; set; }
        public long SizeBytes { get; set; }
        public string SegmentMap { get; set; } = "";
    }

    public sealed class EpisodeEntry
    {
        public string File { get; set; } = "";
        public int? Episode { get; set; }
        public int SourceTitleId { get; set; }
        public int FirstChapter { get; set; }
        public int LastChapter { get; set; }
        /// <summary>The episode's title from the online lookup.</summary>
        public string? Title { get; set; }
    }

    public string Format { get; set; } = "bromelia-archive";
    public int Version { get; set; } = 2;
    /// <summary><c>success</c>, or <c>errors</c> when MakeMKV reported read errors (the files may be damaged).</summary>
    public string Status { get; set; } = "success";
    public string Name { get; set; } = "";
    public string Kind { get; set; } = "";
    public DiscEntry Disc { get; set; } = new();
    public string Rip { get; set; } = "";
    public string Mode { get; set; } = "";
    public string Source { get; set; } = "";
    public string DriveName { get; set; } = "";
    public string Makemkv { get; set; } = "";
    public string JobId { get; set; } = "";
    public DateTime? StartedAt { get; set; }
    public DateTime? FinishedAt { get; set; }
    public List<TitleEntry> Titles { get; set; } = new();
    public List<EpisodeEntry> Episodes { get; set; } = new();
    public List<Checksums.Entry> Files { get; set; } = new();
    public int Warnings { get; set; }
    public int Errors { get; set; }
    public List<string> ErrorMessages { get; set; } = new();
    /// <summary>Errors MakeMKV reported while reading the disc for the rip or backup.</summary>
    public List<string> ReadErrors { get; set; } = new();
    /// <summary>"Using LibreDrive mode (…)" details when the drive read the disc in LibreDrive mode.</summary>
    public string? LibreDrive { get; set; }

    public string ToJson() => JsonSerializer.Serialize(this, new JsonSerializerOptions
    {
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        WriteIndented = true,
        DefaultIgnoreCondition = System.Text.Json.Serialization.JsonIgnoreCondition.WhenWritingNull,
    });
}

/// <summary>Recognises a disc: "v1:" + 32 hex digits of SHA-256 over the volume name, the title count and, sorted, each
/// title's source title id, length in seconds, segment map and size in bytes. The same on every platform
/// (shared/fixtures/fingerprints.json).</summary>
public static class DiscFingerprint
{
    public static string? Of(DiscInfo? info)
    {
        if (info == null || info.Titles.Count == 0) return null;
        var volume = info.VolumeName.Length > 0 ? info.VolumeName : info.Name;
        var lines = info.Titles.Select(t => $"{t.SourceTitleId ?? -1}|{t.DurationSeconds}|{t.SegmentMap}|{t.SizeBytes}")
            .OrderBy(l => l, StringComparer.Ordinal);
        var text = new StringBuilder($"bromelia-disc-fingerprint 1\nvolume:{volume}\ntitles:{info.Titles.Count}\n");
        foreach (var l in lines) text.Append(l).Append('\n');
        var hash = Convert.ToHexString(System.Security.Cryptography.SHA256.HashData(Encoding.UTF8.GetBytes(text.ToString()))).ToLowerInvariant();
        return "v1:" + hash[..32];
    }
}

/// <summary>Where a disc was archived before.</summary>
public sealed record ArchivedMatch(string Folder, DateTime? ArchivedAt);

/// <summary>A finished job that may have archived a disc (from the history).</summary>
public sealed record ArchivedCandidate(string Fingerprint, string Folder, JobState State, DateTime? FinishedAt);

public static class ArchiveLookup
{
    /// <summary>The first candidate (newest first) that archived <paramref name="fingerprint"/> successfully and whose folder still
    /// exists; else, with a root, the first bromelia*.json with status "success" and that fingerprint in root or up to four
    /// folders below it.</summary>
    public static ArchivedMatch? Find(string? fingerprint, IEnumerable<ArchivedCandidate> candidates, string? root)
    {
        if (string.IsNullOrEmpty(fingerprint)) return null;
        if (candidates.FirstOrDefault(c => c.Fingerprint == fingerprint && c.State == JobState.Succeeded && Directory.Exists(c.Folder)) is { } hit)
            return new ArchivedMatch(hit.Folder, hit.FinishedAt);
        return string.IsNullOrEmpty(root) ? null : Scan(root, fingerprint, 0);
    }

    public static bool IsRecordName(string name) => name.StartsWith("bromelia", StringComparison.Ordinal) && name.EndsWith(".json", StringComparison.Ordinal);

    static ArchivedMatch? Scan(string dir, string fingerprint, int depth)
    {
        IEnumerable<string> entries;
        try { entries = Directory.EnumerateFileSystemEntries(dir).ToList(); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return null; }
        var subdirs = new List<string>();
        foreach (var path in entries)
        {
            var name = System.IO.Path.GetFileName(path);
            if (name.StartsWith('.')) continue;
            if (IsRecordName(name) && File.Exists(path))
            {
                if (RecordMatches(path, fingerprint, out var when)) return new ArchivedMatch(dir, when);
            }
            else if (depth < 4 && Directory.Exists(path) && !new DirectoryInfo(path).Attributes.HasFlag(FileAttributes.ReparsePoint))
            {
                subdirs.Add(path);
            }
        }
        foreach (var sub in subdirs.OrderBy(p => p, StringComparer.Ordinal))
            if (Scan(sub, fingerprint, depth + 1) is { } m) return m;
        return null;
    }

    static bool RecordMatches(string path, string fingerprint, out DateTime? when)
    {
        when = null;
        try
        {
            using var doc = JsonDocument.Parse(File.ReadAllText(path));
            var o = doc.RootElement;
            if (o.ValueKind != JsonValueKind.Object || Str(o, "format") != "bromelia-archive" || Str(o, "status") != "success") return false;
            if (!o.TryGetProperty("disc", out var disc) || disc.ValueKind != JsonValueKind.Object || Str(disc, "fingerprint") != fingerprint) return false;
            if (DateTime.TryParse(Str(o, "finishedAt"), System.Globalization.CultureInfo.InvariantCulture,
                    System.Globalization.DateTimeStyles.AdjustToUniversal | System.Globalization.DateTimeStyles.AssumeUniversal, out var t))
                when = t;
            return true;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or JsonException) { return false; }
    }

    static string? Str(JsonElement o, string name) =>
        o.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.String ? v.GetString() : null;
}

/// <summary>Where a TV disc's episode numbering continues: after the last episode of the previous disc of its set (same show,
/// season, part and volume; disc number one lower), read from the archive records (bromelia*.json) of the output folder, or,
/// in a media server library, from the episode numbers already in the season folder.</summary>
public static class EpisodeContinuation
{
    /// <summary>The show's name (after the online lookup), the title read from the disc label, and the disc's place in its set.</summary>
    public sealed record Query(string Name, string LabelTitle, int? Season, int? Part, int? Volume, int Disc);

    /// <summary>The previous disc's last episode, and where it was found (for the log).</summary>
    public sealed record Found(int LastEpisode, string Source);

    /// <summary>An archived disc of a TV show: its place in the set and its highest episode number.</summary>
    public sealed record Record(string Name, string LabelTitle, int? Season, int? Part, int? Volume, int? Disc, int? LastEpisode, string Folder);

    /// <summary>Records are read from <paramref name="folders"/> (the history's) and from <paramref name="root"/> and up to four
    /// folders below it. The season folder is used only when no record is found for the previous disc and none for a later
    /// disc (which would mean the discs were ripped out of order).</summary>
    public static Found? Find(Query q, string? root, IEnumerable<string> folders, string? seasonFolder, int season)
    {
        if (q.Disc <= 1) return null;
        var records = new List<Record>();
        var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        void Add(string path)
        {
            if (seen.Add(Path.GetFullPath(path)) && Read(path) is { } r) records.Add(r);
        }
        foreach (var f in folders)
        {
            try { foreach (var p in Directory.EnumerateFiles(f).Where(p => ArchiveLookup.IsRecordName(Path.GetFileName(p)))) Add(p); }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException or ArgumentException) { }
        }
        if (!string.IsNullOrEmpty(root)) Walk(root, 0, Add);
        var same = records.Where(r => SameSet(r, q)).ToList();
        if (same.Where(r => r.Disc == q.Disc - 1 && r.LastEpisode != null).OrderByDescending(r => r.LastEpisode).FirstOrDefault() is { } prev)
            return new(prev.LastEpisode!.Value, $"disc {q.Disc - 1}, archived in {prev.Folder}");
        if (same.Any(r => (r.Disc ?? 0) > q.Disc) || seasonFolder == null || HighestEpisode(seasonFolder, season) is not { } last) return null;
        return new(last, $"the highest episode in {seasonFolder}");
    }

    public static bool SameSet(Record r, Query q)
    {
        string name = MetadataLookup.Normalize(q.Name), title = MetadataLookup.Normalize(q.LabelTitle);
        var sameShow = (name.Length > 0 && MetadataLookup.Normalize(r.Name) == name) || (title.Length > 0 && MetadataLookup.Normalize(r.LabelTitle) == title);
        return sameShow && r.Season == q.Season && r.Part == q.Part && r.Volume == q.Volume;
    }

    static string? Str(JsonElement o, string name) => o.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.String ? v.GetString() : null;
    static int? Int(JsonElement o, string name) => o.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.Number && v.TryGetInt32(out var i) ? i : null;

    /// <summary>A bromelia*.json of a TV show that was archived (status success or errors), or null.</summary>
    public static Record? Read(string path)
    {
        try
        {
            using var doc = JsonDocument.Parse(File.ReadAllText(path));
            var o = doc.RootElement;
            if (o.ValueKind != JsonValueKind.Object || Str(o, "format") != "bromelia-archive" || Str(o, "status") is not ("success" or "errors")
                || Str(o, "kind") != "tv" || !o.TryGetProperty("disc", out var disc) || disc.ValueKind != JsonValueKind.Object) return null;
            var volumeName = Str(disc, "volumeName") ?? "";
            var label = LabelParser.Parse(volumeName.Length > 0 ? volumeName : Str(disc, "label") ?? "");
            int? last = null;
            if (o.TryGetProperty("episodes", out var eps) && eps.ValueKind == JsonValueKind.Array)
                foreach (var e in eps.EnumerateArray())
                    if (e.ValueKind == JsonValueKind.Object && Int(e, "episode") is { } n && (last == null || n > last)) last = n;
            return new(Str(o, "name") ?? "", label.Title, Int(disc, "season"), Int(disc, "part"), Int(disc, "volume"), Int(disc, "disc"), last,
                Path.GetDirectoryName(path) ?? "");
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or JsonException) { return null; }
    }

    static void Walk(string dir, int depth, Action<string> visit)
    {
        List<string> entries;
        try { entries = Directory.EnumerateFileSystemEntries(dir).OrderBy(p => p, StringComparer.Ordinal).ToList(); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return; }
        foreach (var path in entries)
        {
            var name = Path.GetFileName(path);
            if (name.StartsWith('.')) continue;
            if (ArchiveLookup.IsRecordName(name) && File.Exists(path)) visit(path);
            else if (depth < 4 && Directory.Exists(path) && !new DirectoryInfo(path).Attributes.HasFlag(FileAttributes.ReparsePoint)) Walk(path, depth + 1, visit);
        }
    }

    /// <summary>The highest episode number of <paramref name="season"/> in the names of the files in <paramref name="folder"/>
    /// (<c>… S02E05 …</c>), or null.</summary>
    public static int? HighestEpisode(string folder, int season)
    {
        IEnumerable<string> names;
        try { names = Directory.EnumerateFileSystemEntries(folder).Select(p => Path.GetFileName(p)).ToList(); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return null; }
        int? best = null;
        foreach (var n in names.Where(n => !n.StartsWith('.')))
        {
            var m = System.Text.RegularExpressions.Regex.Match(n, @"[Ss](\d{1,3})[Ee](\d{1,4})");
            if (m.Success && int.Parse(m.Groups[1].Value, CultureInfo.InvariantCulture) == season)
            {
                var e = int.Parse(m.Groups[2].Value, CultureInfo.InvariantCulture);
                if (best == null || e > best) best = e;
            }
        }
        return best;
    }
}

/// <summary>Reads archive folders again and compares every file with its SHA256SUMS (bit rot, bad copies).</summary>
public static class ArchiveVerifier
{
    public sealed class FolderCheck
    {
        public string Folder { get; init; } = "";
        /// <summary>Entries in SHA256SUMS.</summary>
        public int Files { get; set; }
        public long Bytes { get; set; }
        public List<string> Missing { get; } = new();
        public List<string> Changed { get; } = new();
        public List<string> Unreadable { get; } = new();
        /// <summary>Files in the folder that SHA256SUMS doesn't list (not an error).</summary>
        public List<string> Extra { get; set; } = new();
        /// <summary>SHA256SUMS couldn't be read.</summary>
        public string? Error { get; set; }

        public bool Ok => Error == null && Missing.Count == 0 && Changed.Count == 0 && Unreadable.Count == 0;

        /// <summary>"12 file(s) OK" / "1 changed, 2 missing of 12 file(s); 1 not listed".</summary>
        public string Summary
        {
            get
            {
                if (Error != null) return Error;
                string s;
                if (Ok) s = $"{Files} file(s) OK";
                else
                {
                    var parts = new List<string>();
                    if (Changed.Count > 0) parts.Add($"{Changed.Count} changed");
                    if (Unreadable.Count > 0) parts.Add($"{Unreadable.Count} unreadable");
                    if (Missing.Count > 0) parts.Add($"{Missing.Count} missing");
                    s = string.Join(", ", parts) + $" of {Files} file(s)";
                }
                if (Extra.Count > 0) s += $"; {Extra.Count} not listed";
                return s;
            }
        }
    }

    static bool Hidden(string path) => System.IO.Path.GetFileName(path).StartsWith('.');

    static bool IsLink(string path)
    {
        try { return new DirectoryInfo(path).Attributes.HasFlag(FileAttributes.ReparsePoint); }
        catch (IOException) { return true; }
    }

    /// <summary>Folders holding a SHA256SUMS: <paramref name="path"/> itself and every folder below it (hidden folders skipped), sorted.</summary>
    public static List<string> Folders(string path)
    {
        var out_ = new List<string>();
        void Walk(string dir)
        {
            if (File.Exists(System.IO.Path.Combine(dir, Checksums.FileName))) out_.Add(dir);
            IEnumerable<string> subs;
            try { subs = Directory.EnumerateDirectories(dir).ToList(); }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return; }
            foreach (var d in subs) if (!Hidden(d) && !IsLink(d)) Walk(d);
        }
        if (Directory.Exists(path)) Walk(path);
        return out_.OrderBy(p => p, StringComparer.Ordinal).ToList();
    }

    /// <summary>Files Bromelia writes next to the archived ones that SHA256SUMS doesn't list.</summary>
    public static bool IsOwnFile(string name) =>
        name == Checksums.FileName || ArchiveLookup.IsRecordName(name) || (name.StartsWith("bromelia-log", StringComparison.Ordinal) && name.EndsWith(".txt", StringComparison.Ordinal))
        || name is "INCOMPLETE.txt" or "READ ERRORS.txt";

    /// <summary>Progress: bytes hashed so far of the total, the folder and the file being read. Return false to stop.</summary>
    public delegate bool Progress(long done, long total, string? folder, string? file);

    static string Full(string folder, string rel) => System.IO.Path.Combine(folder, rel.Replace('/', System.IO.Path.DirectorySeparatorChar));

    /// <summary>Re-hashes every file of every folder (blocking; call on a worker thread). Returns the folders finished and
    /// whether <paramref name="progress"/> asked to stop.</summary>
    public static (List<FolderCheck> Results, bool Stopped) Verify(IReadOnlyList<string> folders, Progress? progress = null)
    {
        var sums = new List<Dictionary<string, string>?>();
        long total = 0;
        foreach (var folder in folders)
        {
            try
            {
                var map = new Dictionary<string, string>();
                foreach (var (hash, path) in Checksums.Parse(File.ReadAllText(System.IO.Path.Combine(folder, Checksums.FileName)))) map[path] = hash;
                sums.Add(map);
                foreach (var p in map.Keys) { var f = new FileInfo(Full(folder, p)); if (f.Exists) total += f.Length; }
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException) { sums.Add(null); }
        }
        bool stopped = !(progress?.Invoke(0, total, null, null) ?? true);
        long done = 0;
        var results = new List<FolderCheck>();
        using var cts = new CancellationTokenSource();
        for (int i = 0; i < folders.Count && !stopped; i++)
        {
            var folder = folders[i];
            var r = new FolderCheck { Folder = folder };
            if (sums[i] is not { } map)
            {
                r.Error = "SHA256SUMS can't be read";
                results.Add(r);
                continue;
            }
            if (map.Count == 0) r.Error = "SHA256SUMS lists no files";
            r.Files = map.Count;
            foreach (var path in map.Keys.OrderBy(p => p, StringComparer.Ordinal))
            {
                var info = new FileInfo(Full(folder, path));
                if (!info.Exists) { r.Missing.Add(path); continue; }
                long before = done;
                try
                {
                    var hash = Checksums.Sha256(info.FullName, n =>
                    {
                        if (!(progress?.Invoke(before + n, total, folder, path) ?? true)) { stopped = true; cts.Cancel(); }
                    }, cts.Token);
                    if (hash != map[path]) r.Changed.Add(path);
                }
                catch (OperationCanceledException) { }
                catch (Exception e) when (e is IOException or UnauthorizedAccessException) { r.Unreadable.Add(path); }
                done = before + info.Length;
                r.Bytes += info.Length;
                if (stopped) break;
            }
            if (stopped) break;
            r.Extra = ExtraFiles(folder, map.Keys.ToHashSet());
            results.Add(r);
        }
        return (results, stopped);
    }

    /// <summary>Visible files under <paramref name="folder"/> that <paramref name="listed"/> doesn't have, skipping folders that are
    /// archives of their own.</summary>
    public static List<string> ExtraFiles(string folder, HashSet<string> listed)
    {
        var out_ = new List<string>();
        void Walk(string rel)
        {
            var dir = rel.Length == 0 ? folder : Full(folder, rel);
            List<string> names;
            try { names = Directory.EnumerateFileSystemEntries(dir).Select(e => System.IO.Path.GetFileName(e)!).Where(n => !n.StartsWith('.')).OrderBy(n => n, StringComparer.Ordinal).ToList(); }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return; }
            foreach (var name in names)
            {
                var r = rel.Length == 0 ? name : rel + "/" + name;
                var full = Full(folder, r);
                if (Directory.Exists(full))
                {
                    if (!IsLink(full) && !File.Exists(System.IO.Path.Combine(full, Checksums.FileName))) Walk(r);
                }
                else if (!listed.Contains(r) && !(rel.Length == 0 && IsOwnFile(name))) out_.Add(r);
            }
        }
        Walk("");
        return out_;
    }
}

/// <summary>When a folder was last verified (archive-checks.json in the data folder).</summary>
public sealed record CheckRecord(DateTime CheckedAt, bool Ok, string Summary);

public static class CheckRecords
{
    public static string FilePath => System.IO.Path.Combine(Paths.AppData, "archive-checks.json");

    public static Dictionary<string, CheckRecord> Load(string? path = null)
    {
        try
        {
            var p = path ?? FilePath;
            return File.Exists(p) ? JsonSerializer.Deserialize<Dictionary<string, CheckRecord>>(File.ReadAllText(p), ConfigJson.Options) ?? new() : new();
        }
        catch (Exception) { return new(); }
    }

    public static void Save(Dictionary<string, CheckRecord> records, string? path = null) =>
        ConfigStore.SaveRaw(path ?? FilePath, JsonSerializer.Serialize(records, ConfigJson.Options));
}

/// <summary>Runs the external tools used to split DVD "play all" titles: mkvextract (chapter times), ffmpeg +
/// tesseract (episode numbers from menu screens) and mkvmerge --split.</summary>
public static class EpisodeSplitter
{
    /// <summary>Finds a tool next to <paramref name="sibling"/>, on PATH or in the usual install folders.</summary>
    public static string? FindTool(string name, string? sibling = null)
    {
        var exe = OperatingSystem.IsWindows() ? name + ".exe" : name;
        var dirs = new List<string>();
        if (sibling != null && System.IO.Path.GetDirectoryName(sibling) is { } sd) dirs.Add(sd);
        dirs.AddRange((Environment.GetEnvironmentVariable("PATH") ?? "").Split(System.IO.Path.PathSeparator));
        if (OperatingSystem.IsWindows())
        {
            foreach (var pf in new[] { Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), Environment.GetFolderPath(Environment.SpecialFolder.ProgramFilesX86) })
            {
                if (string.IsNullOrEmpty(pf)) continue;
                dirs.Add(System.IO.Path.Combine(pf, "MKVToolNix"));
                dirs.Add(System.IO.Path.Combine(pf, "Tesseract-OCR"));
                dirs.Add(System.IO.Path.Combine(pf, "ffmpeg", "bin"));
            }
            var local = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
            if (local.Length > 0) dirs.Add(System.IO.Path.Combine(local, "Microsoft", "WinGet", "Links"));
        }
        else dirs.AddRange(new[] { "/opt/homebrew/bin", "/usr/local/bin", "/usr/bin" });
        foreach (var d in dirs.Where(d => d.Length > 0))
        {
            var p = System.IO.Path.Combine(d, exe);
            if (File.Exists(p)) return p;
        }
        return null;
    }

    /// <summary>Root folder of a disc in a drive: "E:" → "E:\"; on other systems the mount point of the device.</summary>
    public static string? MountPoint(string device)
    {
        if (Regex.IsMatch(device, @"^[A-Za-z]:\\?$")) return device.TrimEnd('\\') + "\\";
        if (File.Exists("/proc/self/mounts"))
            foreach (var line in File.ReadLines("/proc/self/mounts"))
            {
                var f = line.Split(' ');
                if (f.Length > 1 && f[0] == device) return f[1].Replace("\\040", " ");
            }
        return null;
    }

    public static async Task<double?> DurationAsync(string file, string mkvmerge)
    {
        var lines = new LineCollector();
        try
        {
            var r = await new ProcessRunner(mkvmerge, new[] { "-J", file }).RunAsync(lines.Add, TimeSpan.FromMinutes(2));
            if (r.ExitCode != 0) return null;
            using var doc = JsonDocument.Parse(lines.Joined);
            return doc.RootElement.GetProperty("container").GetProperty("properties").GetProperty("duration").GetDouble() / 1e9;
        }
        catch (Exception e) when (e is JsonException or KeyNotFoundException or InvalidOperationException or System.ComponentModel.Win32Exception) { return null; }
    }

    /// <summary>Chapter start times (seconds) read with <c>mkvextract chapters --simple</c>.</summary>
    public static async Task<List<double>?> ChapterStartsAsync(string file, string? mkvextract)
    {
        if (mkvextract == null) return null;
        var tmp = System.IO.Path.Combine(System.IO.Path.GetTempPath(), $"bromelia-chapters-{Guid.NewGuid():N}.txt");
        try
        {
            var r = await new ProcessRunner(mkvextract, new[] { file, "chapters", "--simple", tmp }).RunAsync(_ => { }, TimeSpan.FromMinutes(2));
            if (r.ExitCode > 1 || !File.Exists(tmp)) return null;
            return ParseSimpleChapters(await File.ReadAllTextAsync(tmp));
        }
        catch (Exception e) when (e is IOException or System.ComponentModel.Win32Exception) { return null; }
        finally { try { File.Delete(tmp); } catch (IOException) { } }
    }

    static readonly Regex ChapterLine = new(@"^CHAPTER\d+=(\d+):(\d+):([\d.]+)", RegexOptions.Multiline);

    public static List<double> ParseSimpleChapters(string text) =>
        ChapterLine.Matches(text).Select(m => int.Parse(m.Groups[1].Value, CultureInfo.InvariantCulture) * 3600
            + int.Parse(m.Groups[2].Value, CultureInfo.InvariantCulture) * 60
            + double.Parse(m.Groups[3].Value, CultureInfo.InvariantCulture)).ToList();

    /// <summary>Reads episode numbers from the menu screens with ffmpeg + tesseract. Returns (null, reason) when
    /// the tools are missing.</summary>
    public static async Task<(HashSet<int>? Numbers, string? Why)> OcrEpisodeNumbersAsync(IVideoTsReader reader, CancellationToken ct)
    {
        var ffmpeg = FindTool("ffmpeg");
        var tesseract = FindTool("tesseract");
        if (ffmpeg == null || tesseract == null) return (null, "ffmpeg and tesseract are needed to read episode numbers from the menus");
        var dir = System.IO.Path.Combine(System.IO.Path.GetTempPath(), $"bromelia-ocr-{Guid.NewGuid():N}");
        Directory.CreateDirectory(dir);
        var found = new HashSet<int>();
        try
        {
            var stills = await Task.Run(() => DvdNavigation.MenuStills(reader), ct);
            for (int i = 0; i < stills.Count && !ct.IsCancellationRequested; i++)
            {
                var mpg = System.IO.Path.Combine(dir, $"menu{i}.mpg");
                var png = System.IO.Path.Combine(dir, $"menu{i}.png");
                await File.WriteAllBytesAsync(mpg, stills[i], ct);
                var ff = await new ProcessRunner(ffmpeg, new[] { "-v", "quiet", "-y", "-f", "mpeg", "-i", mpg, "-frames:v", "1",
                    "-vf", "scale=2160:1440,format=gray", png }).RunAsync(_ => { }, TimeSpan.FromMinutes(1), ct);
                if (ff.ExitCode != 0 || !File.Exists(png)) continue;
                var text = new LineCollector();
                await new ProcessRunner(tesseract, new[] { png, "stdout", "--psm", "11" }).RunAsync(text.Add, TimeSpan.FromMinutes(1), ct);
                found.UnionWith(DvdNavigation.EpisodeNumbersInText(text.Joined));
            }
        }
        catch (Exception e) when (e is IOException or System.ComponentModel.Win32Exception) { }
        finally { try { Directory.Delete(dir, true); } catch (IOException) { } }
        return (found, null);
    }

    /// <summary>Splits <paramref name="file"/> at the given MKV chapters without re-encoding. Returns the parts in
    /// order (hidden temporary names in the same folder), or null when mkvmerge failed.</summary>
    public static async Task<List<string>?> SplitAsync(string file, IReadOnlyList<int> chapters, string mkvmerge,
        Action<ProcessRunner?> register, Action<string> log)
    {
        var dir = System.IO.Path.GetDirectoryName(file)!;
        var prefix = $".bromelia-split-{Guid.NewGuid().ToString("N")[..8]}";
        var runner = new ProcessRunner(mkvmerge, new[] { "-o", System.IO.Path.Combine(dir, prefix + "-%03d.mkv"),
            "--split", "chapters:" + string.Join(",", chapters), file });
        log("$ " + runner.CommandLine);
        register(runner);
        ProcessRunner.Result? r = null;
        try { r = await runner.RunAsync(_ => { }); }
        catch (Exception e) when (e is System.ComponentModel.Win32Exception or InvalidOperationException) { }
        finally { register(null); }
        var parts = Directory.EnumerateFiles(dir, prefix + "*").OrderBy(p => p, StringComparer.Ordinal).ToList();
        // mkvmerge exits with 1 for warnings; the output is still valid.
        if (r is not { ExitCode: <= 1 } || parts.Count != chapters.Count + 1)
        {
            foreach (var p in parts) try { File.Delete(p); } catch (IOException) { }
            return null;
        }
        return parts;
    }
}

/// <summary>Checks a ripped MKV against the title in the disc listing, using <c>mkvmerge -J</c>.</summary>
public static class RipVerifier
{
    public sealed record Probe(double? DurationSeconds, List<string> TrackTypes, int ChapterCount);

    public sealed class Result
    {
        /// <summary>Reasons the file must not be trusted (wrong or truncated title, unreadable file).</summary>
        public List<string> Problems { get; } = new();
        /// <summary>Differences worth noting that don't make the file unusable.</summary>
        public List<string> Notes { get; } = new();
    }

    /// <summary>Allowed difference between the listed and the actual duration: 5 s or 0.5 %, whichever is larger.</summary>
    public static double DurationTolerance(double expected) => Math.Max(5, expected * 0.005);

    /// <summary>Parses <c>mkvmerge -J</c> output. Returns null when mkvmerge did not recognise the file.</summary>
    public static Probe? Parse(string json)
    {
        try
        {
            using var doc = JsonDocument.Parse(json);
            var root = doc.RootElement;
            if (!root.TryGetProperty("container", out var container) || container.ValueKind != JsonValueKind.Object) return null;
            if (container.TryGetProperty("recognized", out var rec) && rec.ValueKind == JsonValueKind.False) return null;
            double? duration = container.TryGetProperty("properties", out var props) && props.TryGetProperty("duration", out var d) && d.TryGetDouble(out var ns)
                ? ns / 1e9 : null;
            var types = root.TryGetProperty("tracks", out var tracks)
                ? tracks.EnumerateArray().Select(t => t.TryGetProperty("type", out var ty) ? ty.GetString() ?? "" : "").ToList()
                : new List<string>();
            int chapters = root.TryGetProperty("chapters", out var ch)
                ? ch.EnumerateArray().Sum(c => c.TryGetProperty("num_entries", out var n) && n.TryGetInt32(out var v) ? v : 0)
                : 0;
            return new Probe(duration, types, chapters);
        }
        catch (JsonException) { return null; }
    }

    public static Result Check(Probe p, TitleInfo title)
    {
        var r = new Result();
        if (p.TrackTypes.Count == 0) r.Problems.Add("the file contains no tracks");
        if (p.TrackTypes.Count > 0 && !p.TrackTypes.Contains("video") && title.Tracks.Any(t => t.Kind == TrackKind.Video))
            r.Problems.Add("the file contains no video track");
        double expected = title.DurationSeconds;
        if (expected > 0)
        {
            if (p.DurationSeconds is { } d)
            {
                if (Math.Abs(d - expected) > DurationTolerance(expected))
                    r.Problems.Add($"it lasts {TitleInfo.FormatDuration((int)Math.Round(d))}, the disc listing says {TitleInfo.FormatDuration((int)expected)}");
            }
            else r.Problems.Add("mkvmerge reports no duration");
        }
        if (title.Tracks.Count > 0 && p.TrackTypes.Count > title.Tracks.Count)
            r.Notes.Add($"{p.TrackTypes.Count} tracks, the disc listing has {title.Tracks.Count}");
        // MakeMKV may add a chapter at 00:00 (profile option), so allow one extra.
        if (title.ChapterCount > 1 && (p.ChapterCount < title.ChapterCount || p.ChapterCount > title.ChapterCount + 1))
            r.Notes.Add($"{p.ChapterCount} chapters, the disc listing has {title.ChapterCount}");
        return r;
    }

    /// <summary>Runs <c>mkvmerge -J</c>. Returns null when the file can't be read.</summary>
    public static async Task<Probe?> ProbeAsync(string mkvmerge, string file, CancellationToken ct = default)
    {
        var lines = new LineCollector();
        try
        {
            var r = await new ProcessRunner(mkvmerge, new[] { "-J", file }).RunAsync(lines.Add, TimeSpan.FromMinutes(5), ct);
            return r.ExitCode <= 1 ? Parse(lines.Joined) : null;
        }
        catch (Exception e) when (e is System.ComponentModel.Win32Exception or IOException) { return null; }
    }
}

/// <summary>Checks that a backup looks like a disc: a folder with a BDMV / VIDEO_TS / HVDVD_TS structure, or an ISO
/// image file (ISO 9660 or UDF volume descriptor at sector 16).</summary>
public static class BackupVerifier
{
    public static string? Problem(string path, bool iso)
    {
        var name = Path.GetFileName(path);
        if (iso)
        {
            if (Directory.Exists(path)) return $"{name} is a folder, not an ISO image";
            if (!File.Exists(path)) return $"{name} was not created";
            try
            {
                using var f = File.OpenRead(path);
                var id = new byte[5];
                f.Seek(32769, SeekOrigin.Begin);
                if (f.Read(id, 0, 5) != 5) return $"{name} is not an ISO / UDF image";
                var s = System.Text.Encoding.ASCII.GetString(id);
                return s is "CD001" or "BEA01" ? null : $"{name} is not an ISO / UDF image";
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return $"{name} can't be read"; }
        }
        if (!Directory.Exists(path)) return File.Exists(path) ? $"{name} is not a folder" : $"{name} was not created";
        string? Sub(string dir, string n) =>
            (Directory.Exists(dir) ? Directory.EnumerateFileSystemEntries(dir) : Enumerable.Empty<string>())
                .FirstOrDefault(e => string.Equals(Path.GetFileName(e), n, StringComparison.OrdinalIgnoreCase));
        if (Sub(path, "BDMV") is { } bdmv) return Sub(bdmv, "index.bdmv") != null ? null : "BDMV/index.bdmv is missing";
        if (Sub(path, "VIDEO_TS") is { } vts) return Sub(vts, "VIDEO_TS.IFO") != null ? null : "VIDEO_TS/VIDEO_TS.IFO is missing";
        if (Sub(path, "HVDVD_TS") != null) return null;
        return "it contains no BDMV, VIDEO_TS or HVDVD_TS folder";
    }
}

/// <summary>Free space on the destination volume.</summary>
public static class DiskSpace
{
    /// <summary>Bytes available to the user on the volume holding <paramref name="path"/>, or null when unknown.</summary>
    public static long? Available(string path)
    {
        string full;
        try { full = Path.GetFullPath(path); }
        catch (Exception e) when (e is ArgumentException or IOException or NotSupportedException) { return null; }
        var cmp = OperatingSystem.IsWindows() ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal;
        DriveInfo? best = null;
        foreach (var d in DriveInfo.GetDrives())
        {
            try
            {
                var root = d.RootDirectory.FullName;
                if (!d.IsReady || !full.StartsWith(root, cmp)) continue;
                if (best == null || root.Length > best.RootDirectory.FullName.Length) best = d;
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
        }
        try { return best?.AvailableFreeSpace; }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return null; }
    }

    /// <summary><paramref name="bytes"/> plus a margin: 2 % or 256 MB, whichever is larger (listing sizes are estimates).</summary>
    public static long Required(long bytes) => bytes + Math.Max(256L << 20, bytes / 50);
}
