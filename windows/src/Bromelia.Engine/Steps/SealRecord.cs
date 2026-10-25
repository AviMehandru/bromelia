using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using Bromelia.Domain;
using Bromelia.Foundation;
using Bromelia.Ports;

namespace Bromelia.Engine;

/// <summary>The archive record SealStep writes (bromelia-&lt;unit8&gt;.json, shared/schema/archive-record-3.json), from what
/// the job's steps found. Messages are kept as codes with parameters and as text in the configured language.</summary>
public static class SealRecord
{
    /// <summary>A version 3 record. status: <paramref name="status"/> when given (KeepApartStep's incomplete), else errors
    /// when MakeMKV reported read errors, else success. disc: the listing
    /// Probe read and the identity Plan resolved (Name's when it corrected it). acquisition: the mode, "Backup" for a backup else "Rip", the job's
    /// source as the request gives it (drive:&lt;id&gt;, iso:&lt;path&gt;, folder:&lt;path&gt;, file:&lt;path&gt;), the drive,
    /// the profile, MakeMKV's version line and the app's. titles: each title Acquire ripped that the listing has, with its
    /// read errors. files: <paramref name="files"/>, as SHA256SUMS lists them. attempts: this job's. counts: the warnings
    /// and errors in the job's log (<paramref name="log"/>). problems: each read error (makemkv.message), then
    /// rip.readErrors. logs: the log files copied into the folder.</summary>
    public static JsonValue Build(JobSnapshot job, PlanOutput plan, AcquireOutput acquired, Id unitId, IReadOnlyList<UnitFile> files,
        IReadOnlyList<LogLineDto> log, IReadOnlyList<string> logs, Instant finishedAt, string appVersion, IMessageRenderer messages, string? status = null)
    {
        var probe = JobSnapshot.Output(job, StepKind.Probe) is JsonValue.Object p ? ProbeOutput.Decode(p) : null;
        var listing = probe?.Listing;
        var identity = NameOutput.Of(job)?.Identity ?? plan.Identity;
        var source = DiscRequest.Decode(job.Request)?.Source;
        var driveId = source is DiscSource.Drive d ? d.DriveId : null;
        int readErrors = AcquireOutput.ReadErrorCount(acquired);
        JsonValue N(int? v) => v is { } x ? JsonValue.Of(x) : JsonValue.Null.Instance;
        var started = Instant.Format(job.StartedAt ?? finishedAt);

        var label = listing is { Name.Length: > 0 } ? listing.Name : source != null ? StepSources.Label(source, null) : "";
        var disc = JsonValue.Of(("label", JsonValue.Of(label)), ("volumeName", JsonValue.Of(listing?.VolumeName ?? "")),
            ("type", JsonValue.Of(listing?.TypeText ?? "")), ("format", JsonValue.Of(EnumWire.Name(identity.Format))),
            ("formatCode", JsonValue.Of(identity.FormatCode.Text)), ("encrypted", JsonValue.Of(identity.Encrypted)),
            ("season", N(identity.Label.Season)), ("part", N(identity.Label.Part)), ("volume", N(identity.Label.Volume)),
            ("disc", N(identity.Label.Disc)), ("set", JsonValue.Of(LabelParser.SetDescription(identity.Label))),
            ("fingerprint", JsonValue.Of(probe?.Fingerprint)), ("libreDrive", JsonValue.Of(probe?.LibreDrive)));

        var acquisition = new List<(string, JsonValue)>
        {
            ("mode", JsonValue.Of(EnumWire.Name(plan.Mode))),
            ("rip", JsonValue.Of(plan.Mode is RipMode.Backup or RipMode.BackupDecrypted ? "Backup" : "Rip")),
            ("source", JsonValue.Of(Source(source))),
        };
        if (driveId != null) acquisition.Add(("drive", JsonValue.Of(("id", JsonValue.Of(driveId)))));
        acquisition.Add(("profile", JsonValue.Of(plan.ProfileId)));
        acquisition.Add(("makemkv", JsonValue.Of(probe?.MakemkvVersion ?? "")));
        acquisition.Add(("bromelia", JsonValue.Of(appVersion)));
        acquisition.Add(("tools", JsonValue.Of(System.Array.Empty<(string, JsonValue)>())));

        var titles = acquired.Titles.Select(t => (Acquired: t, Listed: listing?.Titles.FirstOrDefault(l => l.Index == t.Index)))
            .Where(t => t.Listed != null).Select(t => JsonValue.Of(("index", JsonValue.Of(t.Acquired.Index)),
                ("sourceTitleId", N(t.Listed!.SourceTitleId)), ("sourceFile", JsonValue.Of(t.Listed.SourceFile)),
                ("duration", JsonValue.Of(t.Listed.Duration)), ("durationSeconds", JsonValue.Of(t.Listed.DurationSeconds)),
                ("chapters", JsonValue.Of(t.Listed.Chapters)), ("sizeBytes", JsonValue.Of(t.Listed.SizeBytes)),
                ("segmentMap", JsonValue.Of(t.Listed.SegmentMap)), ("readErrors", JsonValue.Of(t.Acquired.ReadErrors.Count))));

        var fileJson = files.Select(f =>
        {
            var members = new List<(string, JsonValue)> { ("path", JsonValue.Of(f.Path)), ("size", JsonValue.Of(f.Size)), ("sha256", JsonValue.Of(f.Sha256)) };
            if (f.Role.Length > 0) members.Add(("role", JsonValue.Of(f.Role)));
            if (f.Title is { } t) members.Add(("title", JsonValue.Of(t)));
            return JsonValue.Of(members.ToArray());
        });

        var attempt = new List<(string, JsonValue)> { ("job", JsonValue.Of(job.Id.Value)) };
        if (driveId != null) attempt.Add(("drive", JsonValue.Of(driveId)));
        attempt.Add(("startedAt", JsonValue.Of(started)));
        attempt.Add(("titles", JsonValue.Of(acquired.Titles.Select(t =>
            (t.Index.ToString(CultureInfo.InvariantCulture), JsonValue.Of(t.ReadErrors.Count))).ToArray())));
        attempt.Add(("kept", JsonValue.Of(acquired.Titles.Select(t => JsonValue.Of(t.Index)))));

        JsonValue Problem(BroMessage m, bool readError)
        {
            var members = new List<(string, JsonValue)> { ("code", JsonValue.Of(MessageCode.Wire(m.Code))), ("params", m.Params),
                ("text", JsonValue.Of(messages.Render(m))) };
            if (readError) members.Add(("readError", JsonValue.Of(true)));
            return JsonValue.Of(members.ToArray());
        }
        var problems = acquired.Titles.SelectMany(t => t.ReadErrors).Concat(acquired.ReadErrors)
            .Select(text => Problem(new BroMessage(MessageCode.MakemkvMessage, Severity.Error, ("text", JsonValue.Of(text))), true)).ToList();
        if (readErrors > 0) problems.Add(Problem(new BroMessage(MessageCode.RipReadErrors, Severity.Error, ("count", JsonValue.Of(readErrors))), false));

        return JsonValue.Of(
            ("format", JsonValue.Of("bromelia-archive")), ("version", JsonValue.Of(3)),
            ("unit", JsonValue.Of(("id", JsonValue.Of(unitId.Value)), ("short", JsonValue.Of(unitId.Value[..8])), ("library", JsonValue.Of(plan.LibraryId)))),
            ("status", JsonValue.Of(status ?? (readErrors > 0 ? "errors" : "success"))),
            ("name", JsonValue.Of(identity.Name)), ("kind", JsonValue.Of(EnumWire.Name(identity.Kind))),
            ("disc", disc), ("acquisition", JsonValue.Of(acquisition.ToArray())),
            ("job", JsonValue.Of(("id", JsonValue.Of(job.Id.Value)), ("startedAt", JsonValue.Of(started)), ("finishedAt", JsonValue.Of(Instant.Format(finishedAt))))),
            ("titles", JsonValue.Of(titles)), ("episodes", JsonValue.Of(System.Array.Empty<JsonValue>())),
            ("files", JsonValue.Of(fileJson)), ("attempts", JsonValue.Of(new[] { JsonValue.Of(attempt.ToArray()) })),
            ("counts", JsonValue.Of(("warnings", JsonValue.Of(log.Count(l => l.Severity == Severity.Warning))),
                ("errors", JsonValue.Of(log.Count(l => l.Severity == Severity.Error))))),
            ("problems", JsonValue.Of(problems)), ("logs", JsonValue.Of(logs.Select(l => JsonValue.Of(l)))));
    }

    /// <summary>The job's source as the record names it: drive:&lt;id&gt; (MakeMKV's numbering of a drive doesn't last), or
    /// iso:, folder:, file: and the path.</summary>
    static string Source(DiscSource? source) => source switch
    {
        DiscSource.Drive d => "drive:" + d.DriveId,
        DiscSource.Iso i => "iso:" + i.Path,
        DiscSource.Folder f => "folder:" + f.Path,
        DiscSource.File f => "file:" + f.Path,
        _ => "",
    };
}
