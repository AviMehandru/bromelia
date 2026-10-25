using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using Bromelia.Domain;
using Bromelia.Foundation;
using Bromelia.Ports;

namespace Bromelia.Engine;

/// <summary>Step 13 (plan §20.1; §22.3 steps 1–3): makes the job's staging folder a sealed unit, so the commit only
/// moves it. What it seals is the produced items by name (review L3-1: the lists, never a count): Name's output, else
/// Acquire's names as they are. An item that isn't in staging, or holds no file (an empty folder, a hidden path, a path
/// through '..'; owner, 2026-10-21), fails with archive.fileMissing before anything is touched, a visible file no step
/// made with archive.unlisted (owner, 2026-10-20), and leftovers of a stopped write are removed. Then, in order:
/// 1. every file is synced to disk, then every folder from staging down to each file (archive.synced): a folder that
///    holds only folders (a backup's) is synced too, so each entry on the way to a file is durable;
/// 2. every file is hashed read back from disk (archive.hashFromDisk, on by default: the cache is bypassed), with
///    archive.hashing progress; a file that can't be read fails with archive.hashFailed;
/// 3. SHA256SUMS, the logs (the job's log.jsonl rendered line by line, MakeMKV's transcripts as they are), the READ
///    ERRORS note when MakeMKV reported read errors, and the record are written, each atomically; a write that fails
///    fails with archive.writeFailed. Bromelia's own files from an earlier attempt that this one doesn't write go.
/// SHA256SUMS and the record are always written; archive.checksums: false is ignored (archive.checksumsRequired) and
/// archive.archiveRecord: false leaves out only the logs (owner, 2026-10-20). Nothing is trusted from an earlier attempt:
/// a rerun syncs and hashes everything again and writes the same files. Read errors give the unit the tag "READ ERRORS"
/// (Commit puts it in _Quarantine/&lt;name&gt; [READ ERRORS], plan §22.2) and the record's status errors. Data: SealOutput.
/// Skipped when Plan or Acquire hasn't run.</summary>
public sealed class SealStep : IStep
{
    public const string ReadErrorsTag = "READ ERRORS";
    public const string ReadErrorsNote = "READ ERRORS.txt";
    /// <summary>The job folder's MakeMKV transcripts (MakemkvService's makemkv.txt; MakeMKV's debug log).</summary>
    public const string Transcript = "makemkv.txt";
    public const string DebugTranscript = "makemkv-debug.txt";
    const int Chunk = 1 << 20;
    const int FileMode = 0x1A4; // 0644

    public StepKind Kind => StepKind.Seal;

    public Resumability Resumability => Resumability.Idempotent;

    public ResourceSet Resources(JobSnapshot job) => ResourceSet.None with
    {
        LibraryWrites = PlanOutput.Of(job) is { LibraryId.Length: > 0 } plan ? new[] { plan.LibraryId } : Array.Empty<string>(),
    };

    public Task<StepReport> Run(StepContext context) => Task.FromResult(Seal(context));

    static StepReport Seal(StepContext context)
    {
        var job = context.Job;
        var fs = context.Fs;
        if (PlanOutput.Of(job) is not { } plan || AcquireOutput.Of(job) is not { } acquired) return StepReport.Skip;
        var staging = context.Library.StagingDirectory(plan.LibraryId, job.Id);
        var archive = plan.Profile["archive"];
        bool hashFromDisk = archive?["hashFromDisk"]?.AsBool ?? true;
        bool copyLogs = archive?["archiveRecord"]?.AsBool ?? true;
        if (archive?["checksums"]?.AsBool == false) context.Log.Log(new BroMessage(MessageCode.ArchiveChecksumsRequired, Severity.Warning));

        // What is to be sealed, and nothing else.
        var named = NameOutput.Of(job)?.Files ?? NameOutput.FromAcquire(acquired);
        var walked = FileTree.Files(fs, staging);
        // An item that isn't there or holds no file (an empty folder, a hidden path, a path through '..') would seal
        // nothing for itself (owner, 2026-10-21): refused before anything is touched.
        foreach (var item in named)
            if (!ArchiveFiles.Expand(new[] { item.Path }, walked).Any(f => !f.EndsWith('/')))
                throw Failure(MessageCode.ArchiveFileMissing, ("file", JsonValue.Of(item.Path)));
        var tree = new List<string>();
        foreach (var f in walked)
        {
            if (!f.EndsWith('/') && ArchiveFiles.IsLeftover(f[(f.LastIndexOf('/') + 1)..])) fs.Remove(staging + "/" + f);
            else tree.Add(f);
        }
        var files = ArchiveFiles.Expand(named.Select(n => n.Path).ToList(), tree).Where(f => !f.EndsWith('/')).ToList();
        var sealedFiles = new HashSet<string>(files, StringComparer.Ordinal);
        foreach (var f in tree)
        {
            bool hidden = f.TrimEnd('/').Split('/').Any(c => c.StartsWith('.'));
            bool own = !f.Contains('/') && ArchiveFiles.IsOwnFile(f);
            if (!f.EndsWith('/') && !hidden && !own && !sealedFiles.Contains(f))
                throw Failure(MessageCode.ArchiveUnlisted, ("file", JsonValue.Of(f)));
        }
        NamedFile ItemOf(string file) => named.First(n => file == n.Path.TrimEnd('/') || file.StartsWith(n.Path.TrimEnd('/') + "/", StringComparison.Ordinal));

        // 1. Durable before it is read back.
        Cancelled(context);
        foreach (var f in files) fs.SyncFile(staging + "/" + f);
        foreach (var folder in files.SelectMany(f => Folders(staging, f)).Distinct().OrderBy(f => f, StringComparer.Ordinal))
            fs.SyncDirectory(folder);
        context.Log.Log(new BroMessage(MessageCode.ArchiveSynced, Severity.Info, ("count", JsonValue.Of(files.Count))));

        // 2. Hashes of what is on disk.
        var sizes = files.Select(f => fs.Stat(staging + "/" + f).Size).ToList();
        long total = Math.Max(1, sizes.Sum()), done = 0;
        var entries = new List<UnitFile>();
        for (int i = 0; i < files.Count; i++)
        {
            Cancelled(context);
            context.Progress.Report((double)done / total, new BroMessage(MessageCode.ArchiveHashing), files[i]);
            var (hash, size) = Hash(context, staging + "/" + files[i], files[i], hashFromDisk);
            var item = ItemOf(files[i]);
            entries.Add(new UnitFile(files[i], size, hash, item.Role ?? "", item.Title));
            done += sizes[i];
        }
        Cancelled(context);

        // 3. Bromelia's own files, each written atomically.
        var unitId = ArchiveFiles.UnitId(job.Id);
        var unit8 = unitId.Value[..8];
        var written = new List<string>();
        void Write(string name, byte[] bytes)
        {
            WriteOwn(context, staging + "/" + name, bytes);
            written.Add(name);
        }
        Write(Sha256Sums.FileName, Encoding.UTF8.GetBytes(Sha256Sums.Render(entries.Select(e => new SumEntry(e.Path, e.Sha256)))));
        context.Log.Log(new BroMessage(MessageCode.ArchiveChecksumsWritten, Severity.Info, ("count", JsonValue.Of(entries.Count)),
            ("file", JsonValue.Of(Sha256Sums.FileName))));

        var (log, logs) = CopyLogs(context, unit8, copyLogs, Write);

        int readErrors = AcquireOutput.ReadErrorCount(acquired);
        string? note = null;
        if (readErrors > 0)
        {
            var errors = acquired.Titles.SelectMany(t => t.ReadErrors).Concat(acquired.ReadErrors)
                .Select(text => new RobotMessage(2003, 0, 0, text, text, new List<string>())).ToList();
            var lines = Notes.Render(job.Id, Outcome.SucceededWithReadErrors,
                new BroMessage(MessageCode.RipReadErrors, Severity.Error, ("count", JsonValue.Of(readErrors))).ToError(), errors, logs);
            Write(ReadErrorsNote, Encoding.UTF8.GetBytes(string.Concat(lines.Select(l => context.Messages.Render(l) + "\n"))));
            note = ReadErrorsNote;
        }

        var record = "bromelia-" + unit8 + ".json";
        var document = SealRecord.Build(job, plan, acquired, unitId, entries, log, logs, context.Clock.Now(), context.AppVersion, context.Messages);
        Write(record, ArchiveRecordCodec.EncodeV3(new ArchiveRecord(3, document, "", "", "", "", "", null, null, null, null, null, null,
            new List<int>(), new List<SumEntry>())));
        context.Log.Log(new BroMessage(MessageCode.ArchiveRecordWritten, Severity.Info, ("file", JsonValue.Of(record))));

        // An earlier attempt's own files that this one didn't write (a note, a log no longer there).
        foreach (var f in tree.Where(f => !f.Contains('/') && ArchiveFiles.IsOwnFile(f) && !written.Contains(f)))
            fs.Remove(staging + "/" + f);
        fs.SyncDirectory(staging);

        return StepReport.Done(SealOutput.Encode(new SealOutput(unitId, readErrors > 0 ? "errors" : "success", entries, record, logs, note,
            readErrors > 0 ? ReadErrorsTag : null)));
    }

    /// <summary>The job's log (its lines, for the record) and, when <paramref name="copy"/>, the copies written through
    /// <paramref name="write"/>: the log as text (bromelia-&lt;unit8&gt;-log.txt) and MakeMKV's transcripts
    /// (makemkv-&lt;unit8&gt;-log.txt, makemkv-&lt;unit8&gt;-debug-log.txt), each when the job folder has it; their
    /// names. KeepApartStep copies them the same way.</summary>
    internal static (IReadOnlyList<LogLineDto> Log, List<string> Logs) CopyLogs(StepContext context, string unit8, bool copy, Action<string, byte[]> write)
    {
        var fs = context.Fs;
        var logFile = context.JobDirectory + "/" + JobLog.FileName;
        IReadOnlyList<LogLineDto> log = fs.Exists(logFile) ? JobLog.Decode(fs.Read(logFile)) : new List<LogLineDto>();
        var logs = new List<string>();
        if (!copy) return (log, logs);
        if (fs.Exists(logFile))
        {
            write("bromelia-" + unit8 + "-log.txt", Encoding.UTF8.GetBytes(LogText(log, context.Messages)));
            logs.Add("bromelia-" + unit8 + "-log.txt");
        }
        foreach (var (from, to) in new[] { (Transcript, "makemkv-" + unit8 + "-log.txt"), (DebugTranscript, "makemkv-" + unit8 + "-debug-log.txt") })
            if (fs.Exists(context.JobDirectory + "/" + from))
            {
                write(to, fs.Read(context.JobDirectory + "/" + from));
                logs.Add(to);
            }
        return (log, logs);
    }

    /// <summary>Writes one of Bromelia's own files atomically (0644); archive.writeFailed {path, reason} when it can't.</summary>
    internal static void WriteOwn(StepContext context, string path, byte[] bytes)
    {
        try
        {
            context.Fs.WriteAtomically(path, bytes, FileMode);
        }
        catch (BroFailure f)
        {
            throw new BroFailure(new BroMessage(MessageCode.ArchiveWriteFailed, Severity.Error, ("path", JsonValue.Of(path)),
                ("reason", JsonValue.Of(Text(context, f.Error)))).ToError(f.Error));
        }
    }

    /// <summary>A file's SHA-256 and size, read through the FileSystem port (from disk when <paramref name="fromDisk"/>);
    /// archive.hashFailed when it can't be read; job.cancelled between two pieces.</summary>
    internal static (string Hash, long Size) Hash(StepContext context, string path, string file, bool fromDisk)
    {
        IByteStream stream;
        try
        {
            stream = context.Fs.OpenForReading(path, fromDisk);
        }
        catch (BroFailure f)
        {
            throw HashFailed(context, file, f.Error);
        }
        try
        {
            var hasher = new Sha256Hasher();
            long size = 0;
            while (true)
            {
                Cancelled(context);
                byte[] piece;
                try
                {
                    piece = stream.Read(Chunk);
                }
                catch (BroFailure f)
                {
                    throw HashFailed(context, file, f.Error);
                }
                if (piece.Length == 0) break;
                hasher.Update(piece);
                size += piece.Length;
            }
            return (hasher.Finish(), size);
        }
        finally
        {
            stream.Close();
        }
    }

    /// <summary>The folders from <paramref name="staging"/> down to the one that holds <paramref name="file"/>.</summary>
    internal static IEnumerable<string> Folders(string staging, string file)
    {
        yield return staging;
        for (int i = file.IndexOf('/'); i >= 0; i = file.IndexOf('/', i + 1)) yield return staging + "/" + file[..i];
    }

    /// <summary>The job's log as text: per line its time (UTC), [DEBUG] / [WARNING] / [ERROR] unless it is info, and the
    /// message in the configured language.</summary>
    public static string LogText(IReadOnlyList<LogLineDto> lines, IMessageRenderer messages) => string.Concat(lines.Select(l =>
        Instant.Format(l.T) + " " + (l.Severity == Severity.Info ? "" : "[" + EnumWire.Name(l.Severity).ToUpperInvariant() + "] ")
        + messages.Render(new BroMessage(l.Code, l.Params, l.Severity)) + "\n"));

    internal static void Cancelled(StepContext context)
    {
        if (context.Cancel.IsCancelled) throw new BroFailure(new BroError(MessageCode.Wire(MessageCode.JobCancelled)));
    }

    static string Text(StepContext context, BroError error) =>
        context.Messages.Render(new BroMessage(MessageCode.Parse(error.Code) ?? MessageCode.InternalUnexpected, error.Params, Severity.Error));

    static BroFailure HashFailed(StepContext context, string file, BroError cause) =>
        new(new BroMessage(MessageCode.ArchiveHashFailed, Severity.Error, ("file", JsonValue.Of(file)), ("reason", JsonValue.Of(Text(context, cause))))
            .ToError(cause));

    static BroFailure Failure(MessageCode code, params (string, JsonValue)[] parameters) =>
        new(new BroMessage(code, Severity.Error, parameters).ToError());
}
