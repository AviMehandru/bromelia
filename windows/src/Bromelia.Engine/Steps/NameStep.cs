using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using Bromelia.Domain;
using Bromelia.Foundation;
using Bromelia.Ports;

namespace Bromelia.Engine;

/// <summary>Step 12 (plan §20.1): decides the unit's folder in the library and every produced item's name in it, and
/// renames inside staging so staging is laid out exactly as the unit's folder will be: Seal hashes it so, Commit moves
/// it.
///
/// Names come from Layouts with the identity's tokens, date tokens in local time at the job's start; a title's
/// hand-given name is its own template. disc-info.json and the files no title owns keep their names. A folder backup's
/// BDMV/index.bdmv or VIDEO_TS can correct the disc's format (a UHD disc read without a listing looks like a Blu-ray):
/// NameOutput then carries the corrected identity for Seal's record.
///
/// Nothing is overwritten: a folder of its own takes a free name ("Name (2)"), a shared folder is merged item by item,
/// and an item whose name is taken takes a free one. An item whose place holds a target, ASCII case ignored (Acquire's
/// "backup" and a media server's "Backup/"), first goes to a free name at staging's root. A name that would hold no
/// file (empty, a component starting with '.', one of Bromelia's own file names at the unit's root) fails before
/// anything is renamed.
///
/// Idempotent: the decisions are saved as the "names" checkpoint before the first rename, so an attempt after a crash
/// or a retry finds each item where it is and keeps the names given, unless one is now taken in the library. A
/// checkpoint for other items than Acquire's (Acquire ran again) is ignored. Data: NameOutput. Skipped when Plan or
/// Acquire hasn't run.</summary>
public sealed class NameStep : IStep
{
    public const string NamesCheckpoint = "names";

    public StepKind Kind => StepKind.Name;

    public Resumability Resumability => Resumability.Idempotent;

    public ResourceSet Resources(JobSnapshot job) => ResourceSet.None with
    {
        LibraryWrites = PlanOutput.Of(job) is { LibraryId.Length: > 0 } plan ? new[] { plan.LibraryId } : Array.Empty<string>(),
    };

    public async Task<StepReport> Run(StepContext context)
    {
        var job = context.Job;
        var fs = context.Fs;
        if (PlanOutput.Of(job) is not { } plan || AcquireOutput.Of(job) is not { } acquired) return StepReport.Skip;
        var library = context.Library.Require(plan.LibraryId).Path;
        var staging = context.Library.StagingDirectory(plan.LibraryId, job.Id);
        var naming = NamingSettings.Decode(plan.Profile["naming"] ?? JsonValue.Of());
        if (plan.Mode != RipMode.BackupThenMkv) naming = naming with { BackupSubfolder = "" };
        var items = NameOutput.FromAcquire(acquired);
        var saved = context.Checkpoint.Saved is JsonValue.Object s && s["kind"]?.AsString == NamesCheckpoint ? s : null;
        // A checkpoint made for other items than Acquire's is no guide: Acquire ran again since (a retry from it or
        // Plan), and removed whatever Name had renamed, so every item is under Acquire's name.
        if (saved != null && !(saved["files"]?.AsArray ?? new List<JsonValue>()).Select(f => f["from"]?.AsString ?? "")
                .ToHashSet(StringComparer.Ordinal).SetEquals(items.Select(i => i.Path))) saved = null;
        var savedFiles = (saved?["files"]?.AsArray ?? new List<JsonValue>())
            .Where(f => f["from"]?.AsString != null).GroupBy(f => f["from"]!.AsString!, StringComparer.Ordinal).ToDictionary(g => g.Key, g => g.First(), StringComparer.Ordinal);

        // 1. Where each produced item is now: where an earlier attempt sent it (its target, or the free name it went to
        // on the way), else where that attempt found it, else under Acquire's name. In that order: a folder made for a
        // target may be where an item was (ASCII case aside), but nothing but the item is ever at its target or its
        // way.
        var at = new List<string>();
        foreach (var item in items)
        {
            var was = savedFiles.GetValueOrDefault(item.Path);
            var found = new[] { was?["to"]?.AsString, was?["via"]?.AsString, was?["at"]?.AsString, item.Path }.FirstOrDefault(p => p is { Length: > 0 } && fs.Exists(staging + "/" + p));
            at.Add(found ?? throw Failure(MessageCode.ArchiveFileMissing, ("file", JsonValue.Of(item.Path))));
        }

        // 2. The format a folder backup shows.
        var identity = plan.Identity;
        Identity? corrected = null;
        int backup = items.ToList().FindIndex(i => i.Role == "backup");
        if (backup >= 0 && fs.Stat(staging + "/" + at[backup]).IsDirectory && BackupFormat(fs, staging + "/" + at[backup]) is { } format
            && format != identity.Format)
        {
            identity = corrected = identity with { Format = format, FormatCode = FormatDetector.Code(format, identity.Encrypted) };
            context.Log.Log(new BroMessage(MessageCode.NameFormatFromBackup, Severity.Info, ("format", JsonValue.Of(identity.FormatCode.Text))));
        }
        var probe = ProbeOutput.Decode(JobSnapshot.Output(job, StepKind.Probe) ?? JsonValue.Null.Instance);
        var values = TokenRegistry.Values(identity, Tokens(context, plan, probe.Listing));

        // 3. The unit's folder.
        var wanted = Layouts.Folder(naming, values);
        if (wanted.Length == 0) throw Failure(MessageCode.NameNoFolder);
        if (AsciiLower(wanted.Split('/')[0]) == "_quarantine") throw Failure(MessageCode.NameUnusable, ("path", JsonValue.Of(wanted)));
        bool merge = naming.Layout == Layout.MediaServer || naming.ConflictPolicy == ConflictPolicy.Merge;
        var folder = wanted;
        if (!merge)
        {
            var kept = saved?["folder"]?.AsString;
            if (kept is { Length: > 0 } && !Taken(fs, library, kept) && (naming.ConflictPolicy != ConflictPolicy.Skip || kept == wanted)) folder = kept;
            else if (Taken(fs, library, wanted))
            {
                if (naming.ConflictPolicy == ConflictPolicy.Skip)
                    throw Failure(MessageCode.LibraryFolderExists, ("path", JsonValue.Of(library + "/" + wanted)));
                folder = Join(Parent(wanted), ConflictNamer.Next(Last(wanted), Names(fs, Join(library, Parent(wanted))), isFolder: true));
            }
        }

        // 4. Every item's name in it: a name kept from an earlier attempt, else a free one: never a name another entry in
        // staging holds, nor another item's Acquire name (where a retry from Acquire puts that item again).
        List<string> InStaging(int i, string dir) =>
            Names(fs, Join(staging, dir)).Where(n => !(Parent(at[i]) == dir && n == Last(at[i])))
                .Concat(items.Where((item, j) => j != i && Parent(item.Path) == dir).Select(item => Last(item.Path))).ToList();
        var desired = Desired(items, at, plan, acquired, probe.Listing, naming, values, wanted);
        var to = new string?[items.Count];
        for (int i = 0; i < items.Count; i++)
            if (savedFiles.GetValueOrDefault(items[i].Path)?["to"]?.AsString is { Length: > 0 } kept && !(merge && Taken(fs, library + "/" + folder, kept)))
                to[i] = kept;
        for (int i = 0; i < items.Count; i++)
        {
            if (to[i] != null) continue;
            var dir = Parent(desired[i]);
            var taken = InStaging(i, dir);
            taken.AddRange(to.Where(t => t != null && Parent(t) == dir).Select(t => Last(t!)));
            if (merge) taken.AddRange(Names(fs, Join(library + "/" + folder, dir)));
            var free = Join(dir, ConflictNamer.Next(Last(desired[i]), taken, fs.Stat(staging + "/" + at[i]).IsDirectory));
            to[i] = AsciiLower(free) == AsciiLower(at[i]) ? at[i] : free; // a case-insensitive disk can't rename to a case alone
        }
        // No item goes inside another item's target. An item whose place holds a target (its own too, ASCII case ignored: a
        // disk may not tell "backup" from "Backup/") first goes to a free name at staging's root, so nothing moves into a
        // folder that moves, nor into itself.
        for (int i = 0; i < items.Count; i++)
            if (Enumerable.Range(0, items.Count).Any(j => j != i && Inside(to[i]!, to[j]!)))
                throw Failure(MessageCode.NameUnusable, ("path", JsonValue.Of(folder + "/" + to[i])));
        var via = new string?[items.Count];
        var root = Names(fs, staging).Concat(items.SelectMany((item, j) => new[] { item.Path, at[j], to[j]! }).Select(First)).ToList();
        for (int j = 0; j < items.Count; j++)
            if (to[j] != at[j] && to.Any(t => Inside(t!, at[j])))
                root.Add(via[j] = ConflictNamer.Next(Last(at[j]), root, fs.Stat(staging + "/" + at[j]).IsDirectory));

        // 5. The decisions are kept before anything moves, then the renames.
        var checkpoint = JsonValue.Of(("kind", JsonValue.Of(NamesCheckpoint)), ("folder", JsonValue.Of(folder)),
            ("files", JsonValue.Of(items.Select((item, i) => JsonValue.Of(new[] { ("from", JsonValue.Of(item.Path)), ("at", JsonValue.Of(at[i])), ("to", JsonValue.Of(to[i])) }
                .Concat(via[i] != null ? new[] { ("via", JsonValue.Of(via[i])) } : Array.Empty<(string, JsonValue)>()).ToArray())))));
        Cancelled(context);
        if (saved == null || !JsonValue.EncodeCanonical(saved).SequenceEqual(JsonValue.EncodeCanonical(checkpoint))) await context.Checkpoint.Save(checkpoint);
        context.Log.Log(new BroMessage(MessageCode.ArchiveOutputFolder, Severity.Info, ("path", JsonValue.Of(library + "/" + folder))));
        for (int i = 0; i < items.Count; i++)
            if (via[i] != null) Rename(context, staging, at[i], via[i]!);
        for (int i = 0; i < items.Count; i++)
            if (to[i] != (via[i] ?? at[i])) Rename(context, staging, via[i] ?? at[i], to[i]!);
        if (corrected != null && to[backup] != at[backup])
            context.Log.Log(new BroMessage(MessageCode.BackupRenamed, Severity.Info, ("name", JsonValue.Of(Last(to[backup]!)))));
        return StepReport.Done(NameOutput.Encode(new NameOutput(folder, Last(folder), merge,
            items.Select((item, i) => item with { Path = to[i]! }).ToList(), corrected)));
    }

    /// <summary>Renames <paramref name="from"/> to <paramref name="to"/> in staging, making the folders it goes in; a cancel
    /// comes first.</summary>
    static void Rename(StepContext context, string staging, string from, string to)
    {
        Cancelled(context);
        var parent = staging;
        foreach (var part in Parent(to).Split('/', StringSplitOptions.RemoveEmptyEntries))
        {
            parent += "/" + part;
            if (!context.Fs.Exists(parent)) context.Fs.CreateDirectory(parent, parentsMustExist: true);
        }
        context.Fs.Rename(staging + "/" + from, staging + "/" + to);
        context.Log.Log(new BroMessage(MessageCode.RipRenamed, Severity.Info, ("from", JsonValue.Of(from)), ("to", JsonValue.Of(to))));
    }

    /// <summary>Each item's wanted path in the unit's folder (Layouts.paths without the folder), or name.unusable.</summary>
    static List<string> Desired(IReadOnlyList<NamedFile> items, IReadOnlyList<string> at, PlanOutput plan, AcquireOutput acquired, Listing? listing,
        NamingSettings naming, IReadOnlyDictionary<string, string> values, string folder)
    {
        var titles = acquired.Titles.Select(t => listing?.Titles.FirstOrDefault(x => x.Index == t.Index)).ToList();
        int? main = titles.OfType<Title>().OrderByDescending(t => t.DurationSeconds).FirstOrDefault()?.Index;
        var outputs = new List<PlannedOutput>();
        var hand = new Dictionary<int, string>();
        foreach (var item in items)
        {
            var ext = Extension(item.Path);
            if (item.Role == "title")
            {
                int n = acquired.Titles.ToList().FindIndex(t => t.Index == item.Title) + 1;
                var t = titles[n - 1];
                var v = new Dictionary<string, string>
                {
                    ["track"] = t != null ? TokenRegistry.TrackLabel(t) : "",
                    ["title"] = t is { Name.Length: > 0 } ? t.Name : listing?.Name ?? "",
                    ["index"] = N(item.Title!.Value), ["n"] = N(n), ["source"] = t?.SourceTitleId is { } src ? N(src) : "",
                    ["duration"] = t is { } d ? string.Format(CultureInfo.InvariantCulture, "{0}-{1:00}-{2:00}", d.DurationSeconds / 3600, d.DurationSeconds / 60 % 60, d.DurationSeconds % 60) : "",
                    ["chapters"] = t != null ? N(t.Chapters) : "", ["original"] = Stem(item.Path), ["comment"] = t?.Comment ?? "",
                };
                if (plan.Titles.FirstOrDefault(c => c.Index == item.Title)?.Name.Trim(AsciiSpace) is { Length: > 0 } name)
                {
                    var all = new Dictionary<string, string>(values);
                    foreach (var kv in v) all[kv.Key] = kv.Value;
                    var file = TemplateEngine.RenderPath(name, all);
                    hand[outputs.Count] = file.Length == 0 || file.EndsWith(".mkv", StringComparison.OrdinalIgnoreCase) ? file : file + ".mkv";
                }
                outputs.Add(new PlannedOutput(PathRole.Title, v, item.Title, MainFeature: item.Title == main, Extension: ext));
            }
            else if (item.Role == "backup")
            {
                var v = new[] { "title", "index", "n", "source", "duration", "chapters", "original", "comment" }.ToDictionary(k => k, _ => "");
                v["rip"] = "Backup";
                outputs.Add(new PlannedOutput(PathRole.Backup, v, Extension: ext));
            }
            else outputs.Add(null!);
        }
        var paths = Layouts.Paths(naming, values, outputs.Where(o => o != null).ToList());
        var result = new List<string>();
        int p = 0;
        for (int i = 0; i < items.Count; i++)
        {
            string rel;
            if (outputs[i] == null) rel = items[i].Path;
            else
            {
                var path = paths[p++].Path;
                rel = hand.TryGetValue(i, out var h) ? h : path.Length > folder.Length ? path[folder.Length..].TrimStart('/') : "";
            }
            var parts = rel.Split('/');
            if (parts.Any(c => c.Length == 0 || c.StartsWith('.')) || (parts.Length == 1 && ArchiveFiles.IsOwnFile(rel)))
                throw Failure(MessageCode.NameUnusable, ("path", JsonValue.Of(folder + "/" + rel)));
            result.Add(rel);
        }
        return result;
    }

    /// <summary>What the tokens shared by every file need besides the identity.</summary>
    static TokenContext Tokens(StepContext context, PlanOutput plan, Listing? listing)
    {
        var request = StepSources.Request(context.Job);
        var drive = request.Source is DiscSource.Drive d ? context.DriveLookup.Find(d.DriveId) : null;
        var driveName = drive?.ConfigId is { } configId ? Config.Drives(context.Config).FirstOrDefault(e => e.Id == configId)?.Name ?? "" : "";
        var label = StepSources.Label(request.Source, drive);
        return new TokenContext(plan.Mode is RipMode.Mkv or RipMode.BackupThenMkv ? "Rip" : "Backup", driveName,
            listing is { Name.Length: > 0 } ? listing.Name : label, listing is { VolumeName.Length: > 0 } ? listing.VolumeName : label,
            listing?.Type ?? DiscType.Disc, context.Job.Id.Value.Replace("-", "")[..8], null,
            context.Clock.LocalTime(context.Job.StartedAt ?? context.Clock.Now()));
    }

    /// <summary>The format a folder backup's structure shows (VIDEO_TS: DVD; BDMV/index.bdmv: UHD or Blu-ray), or
    /// null.</summary>
    static DiscFormat? BackupFormat(IFileSystem fs, string backup)
    {
        var index = backup + "/BDMV/index.bdmv";
        var text = fs.Exists(index) ? Encoding.ASCII.GetString(fs.ReadRange(index, 0, 8)) : null;
        var format = FormatDetector.Detect(null, null, text, fs.Exists(backup + "/VIDEO_TS"));
        return format == DiscFormat.Unknown ? null : format;
    }

    /// <summary>Whether <paramref name="relative"/> is taken under <paramref name="root"/>, ignoring ASCII case as
    /// ConflictNamer does.</summary>
    static bool Taken(IFileSystem fs, string root, string relative) =>
        ConflictNamer.Next(Last(relative), Names(fs, Join(root, Parent(relative)))) != Last(relative);

    /// <summary>The names in a folder; none when it isn't there.</summary>
    static List<string> Names(IFileSystem fs, string folder) =>
        fs.Exists(folder) && fs.Stat(folder).IsDirectory ? fs.List(folder).Select(e => e.Name).ToList() : new List<string>();

    /// <summary>Whether <paramref name="path"/> lies inside the folder <paramref name="folder"/>, ignoring ASCII
    /// case.</summary>
    static bool Inside(string path, string folder) => AsciiLower(path).StartsWith(AsciiLower(folder) + "/", StringComparison.Ordinal);

    static string Parent(string path) => path.LastIndexOf('/') is var i and >= 0 ? path[..i] : "";

    static string First(string path) => path.Split('/')[0];

    static string Last(string path) => path[(path.LastIndexOf('/') + 1)..];

    static string Join(string a, string b) => a.Length == 0 ? b : b.Length == 0 ? a : a + "/" + b;

    static string Extension(string path) => Last(path).LastIndexOf('.') is var dot and > 0 ? Last(path)[dot..] : "";

    static string Stem(string path) => Last(path)[..(Last(path).Length - Extension(path).Length)];

    /// <summary>The white space trimmed from a hand-given name's ends: ASCII only (C's g_strstrip), so a no-break space
    /// stays on all three platforms.</summary>
    static readonly char[] AsciiSpace = { ' ', '\t', '\n', '\v', '\f', '\r' };

    /// <summary>Upper-case ASCII letters lowered, as ConflictNamer compares names.</summary>
    static string AsciiLower(string s) => string.Concat(s.Select(c => c is >= 'A' and <= 'Z' ? (char)(c + 32) : c));

    static string N(int value) => value.ToString(CultureInfo.InvariantCulture);

    static void Cancelled(StepContext context)
    {
        if (context.Cancel.IsCancelled) throw new BroFailure(new BroError(MessageCode.Wire(MessageCode.JobCancelled)));
    }

    static BroFailure Failure(MessageCode code, params (string, JsonValue)[] parameters) =>
        new(new BroMessage(code, Severity.Error, parameters).ToError());
}
