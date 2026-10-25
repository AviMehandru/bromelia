using Bromelia.Domain;
using Bromelia.Foundation;
using Bromelia.Ports;
using Xunit;
using static Bromelia.Tests.Fixtures;
using CancellationToken = Bromelia.Foundation.CancellationToken;

namespace Bromelia.Engine.Tests;

/// <summary>shared/fixtures/engine/commit-steps.cases.json: CommitStep (or NotifyStep) alone, with a fake library port
/// that records what it is asked to commit, a fixed clock, a log and progress. The format is in
/// claudeDocs/tools/rebuild-phase3/gen-commit-cases.py.</summary>
public sealed class CommitStepTests
{
    const string JobId = "00000000-0000-4000-8000-000000000001";
    static JsonValue.Object EmptyObject => new(new List<KeyValuePair<string, JsonValue>>());

    sealed class Fakes : IProgressReporter, IJobLogger, ILibraryPort, IClock
    {
        readonly JsonValue _given;
        public readonly List<BroMessage> Log = new();
        public readonly List<JsonValue> Progress = new();
        public readonly List<JsonValue> Calls = new();
        public int StagingAsked;
        /// <summary>Set when the case cancels the job as the port is called.</summary>
        public CancellationSource? CancelOnCall;

        public Fakes(JsonValue given) { _given = given; }

        public void Report(double fraction, BroMessage operation, string? current = null)
        {
            var item = new List<JsonValue> { new JsonValue.Number(fraction), JsonValue.Of(MessageCode.Wire(operation.Code)) };
            if (current != null) item.Add(JsonValue.Of(current));
            lock (Progress) Progress.Add(JsonValue.Of(item));
        }

        void IJobLogger.Log(BroMessage message)
        {
            lock (Log) Log.Add(message);
        }

        public LibraryHandle Require(string libraryId)
        {
            if (_given["libraries"]?[libraryId] is not { } l) throw new BroFailure(new BroError(MessageCode.Wire(MessageCode.LibraryNotFound)));
            if (l["offline"] is { } why) throw new BroFailure(new BroError(why["code"]!.AsString!, (JsonValue.Object)(why["params"] ?? EmptyObject)));
            return new LibraryHandle(libraryId, l["path"]!.AsString!);
        }

        public string StagingDirectory(string libraryId, Id jobId)
        {
            var path = Require(libraryId).Path + "/.bromelia/staging/" + jobId.Value;
            StagingAsked++;
            return path;
        }

        public Task<CommitResult> Commit(UnitRecord unit, CommitIntent intent, IReadOnlyList<UnitFile> files, CancellationToken cancel) =>
            Task.FromResult(Called("commit", unit, intent, files, null, cancel));

        public Task<CommitResult> Quarantine(UnitRecord unit, CommitIntent intent, IReadOnlyList<UnitFile> files, string tag, CancellationToken cancel) =>
            Task.FromResult(Called("quarantine", unit, intent, files, tag, cancel));

        CommitResult Called(string op, UnitRecord unit, CommitIntent intent, IReadOnlyList<UnitFile> files, string? tag, CancellationToken cancel)
        {
            var call = new List<(string, JsonValue)> { ("op", JsonValue.Of(op)) };
            if (tag != null) call.Add(("tag", JsonValue.Of(tag)));
            call.Add(("unit", UnitJson(unit)));
            call.Add(("intent", IntentJson(intent)));
            call.Add(("files", JsonValue.Of(files.Select(f => JsonValue.Of(new[]
            {
                JsonValue.Of(f.Path), JsonValue.Of(f.Size), JsonValue.Of(f.Sha256), JsonValue.Of(f.Role),
                f.Title is { } t ? JsonValue.Of(t) : JsonValue.Null.Instance,
            })))));
            lock (Calls) Calls.Add(JsonValue.Of(call.ToArray()));
            CancelOnCall?.Cancel();
            // LibraryService ends its wait for the unit so when the token it was given is cancelled.
            if (cancel.IsCancelled) throw new BroFailure(new BroError(MessageCode.Wire(MessageCode.JobCancelled)));
            var port = _given["port"];
            if (port?["error"] is { } e) throw new BroFailure(new BroError(e["code"]!.AsString!, (JsonValue.Object)(e["params"] ?? EmptyObject)));
            var parent = intent.Destination[..intent.Destination.LastIndexOf('/')];
            var destination = port?["destination"]?.AsString
                ?? (tag != null ? parent + "/" + intent.Destination[(parent.Length + 1)..] + " [" + tag + "]" : intent.Destination);
            return new CommitResult(unit.Id, destination, files, port?["merged"]?.AsBool ?? (tag == null && intent.Merge));
        }

        public Instant Now() => Instant.Parse(_given["now"]!.AsString!)!.Value;

        public Duration Monotonic() => throw new NotSupportedException();

        public Task Sleep(Duration duration, CancellationToken cancel) => throw new NotSupportedException();

        public ITimerHandle Timer(TimerSchedule schedule, Action handler) => throw new NotSupportedException();

        public LocalTime LocalTime(Instant instant) => throw new NotSupportedException();
    }

    static JsonValue N(int? v) => v is { } x ? JsonValue.Of(x) : JsonValue.Null.Instance;

    static JsonValue UnitJson(UnitRecord u) => JsonValue.Of(
        ("id", JsonValue.Of(u.Id.Value)), ("libraryId", JsonValue.Of(u.LibraryId)), ("physicalDiscId", JsonValue.Of(u.PhysicalDiscId?.Value)),
        ("jobId", JsonValue.Of(u.JobId?.Value)), ("path", JsonValue.Of(u.Path)), ("recordFile", JsonValue.Of(u.RecordFile)),
        ("recordVersion", JsonValue.Of(u.RecordVersion)), ("state", JsonValue.Of(EnumWire.Name(u.State))), ("status", JsonValue.Of(EnumWire.Name(u.Status))),
        ("name", JsonValue.Of(u.Name)), ("kind", JsonValue.Of(EnumWire.Name(u.Kind))), ("format", JsonValue.Of(u.Format)),
        ("formatCode", JsonValue.Of(u.FormatCode)), ("encrypted", JsonValue.Of(u.Encrypted)), ("fingerprint", JsonValue.Of(u.Fingerprint)),
        ("label", JsonValue.Of(u.Label)), ("season", N(u.Season)), ("part", N(u.Part)), ("volume", N(u.Volume)), ("disc", N(u.Disc)),
        ("makemkvVersion", JsonValue.Of(u.MakemkvVersion)), ("bytes", JsonValue.Of(u.Bytes)), ("fileCount", JsonValue.Of(u.FileCount)),
        ("attempts", JsonValue.Of(u.Attempts)), ("createdAt", JsonValue.Of(Instant.Format(u.CreatedAt))),
        ("committedAt", u.CommittedAt is { } c ? JsonValue.Of(Instant.Format(c)) : JsonValue.Null.Instance));

    static JsonValue IntentJson(CommitIntent i) => JsonValue.Of(
        ("unitId", JsonValue.Of(i.UnitId.Value)), ("jobId", JsonValue.Of(i.JobId.Value)), ("staging", JsonValue.Of(i.Staging)),
        ("destination", JsonValue.Of(i.Destination)), ("merge", JsonValue.Of(i.Merge)), ("quarantine", JsonValue.Of(i.Quarantine)),
        ("createdAt", JsonValue.Of(Instant.Format(i.CreatedAt))),
        ("items", JsonValue.Of(i.Items.Select(x => JsonValue.Of(new[]
        {
            JsonValue.Of(x.Seq), JsonValue.Of(x.FromPath), JsonValue.Of(x.ToPath), JsonValue.Of(x.Sha256), JsonValue.Of(x.IsDir), JsonValue.Of(x.Moved),
        })))));

    static JobRequest RequestOf(JsonValue r) => new(
        EnumWire.Parse<JobKind>(r["kind"]?.AsString) ?? JobKind.VideoDisc, r["title"]?.AsString ?? "", r["automatic"]?.AsBool ?? false,
        r["driveId"]?.AsString, r["mediaGeneration"]?.AsInteger, null, Options: r["options"], Details: r["details"]);

    static void SameError(JsonValue expected, BroError? error, string what)
    {
        Same(expected["code"]!.AsString, error?.Code, what);
        Same(expected["params"] ?? EmptyObject, (JsonValue?)error?.Params ?? EmptyObject, what + " params");
    }

    [Fact]
    public void TheCommitCasesPass() => RunCases("engine/commit-steps.cases.json", Case);

    static bool Case(string id, JsonValue given, JsonValue expect)
    {
        IStep step = given["step"]?.AsString == "notify" ? new NotifyStep() : new CommitStep();
        var fakes = new Fakes(given);
        var cancel = new CancellationSource();
        if (given["cancelAt"]?["op"]?.AsString == "start") cancel.Cancel();
        else if (given["cancelAt"]?["op"]?.AsString == "commit") fakes.CancelOnCall = cancel;
        var outputs = (given["outputs"]?.AsArray ?? new List<JsonValue>()).Select(o =>
            new StepOutput(EnumWire.Parse<StepKind>(o["kind"]!.AsString)!.Value, o["data"]!, null, (int)(o["readErrors"]?.AsInteger ?? 0))).ToList();
        var job = new JobSnapshot(new Id(JobId), RequestOf(given["request"]!), null, outputs, null);

        if (expect["resources"] is { } resources)
            Same(resources["libraryWrites"], JsonValue.Of(step.Resources(job).LibraryWrites.Select(d => JsonValue.Of(d))), "resources.libraryWrites");
        if (expect["resumes"] is { } resumes) Same(resumes["resumability"]!.AsString, EnumWire.Name(step.Resumability), "resumability");

        var context = new StepContext(job, cancel.Token, fakes, fakes, null!, new ToolSet(null!, null!), null!, fakes, null!, fakes, null!,
            new Config(JsonValue.Of(), new List<Issue>()), null!, null!, null!, null!, "/data/jobs/" + JobId, null!, "test");
        StepReport? report = null;
        BroError? thrown = null;
        try
        {
            report = Task.Run(() => step.Run(context)).GetAwaiter().GetResult();
        }
        catch (BroFailure f)
        {
            thrown = f.Error;
        }

        var error = thrown ?? report?.Error;
        if (expect["error"] is { } expectedError) SameError(expectedError, error, "error");
        else if (error != null) throw new Xunit.Sdk.XunitException($"unexpected error {error.Code} {error.Params}");
        Same(expect["skipped"]?.AsBool ?? false, report?.Skipped ?? false, "skipped");
        Same(expect["quarantined"]?.AsBool ?? false, report?.Quarantined ?? false, "quarantined");
        if (expect["data"] is { } data) Same(data, report?.Data ?? JsonValue.Null.Instance, "data");
        if (expect["log"] is { } log)
        {
            Same(log.AsArray!.Count, fakes.Log.Count, "log length (" + string.Join(", ", fakes.Log.Select(m => MessageCode.Wire(m.Code))) + ")");
            for (int i = 0; i < fakes.Log.Count; i++)
            {
                Same(log.AsArray[i]["code"]!.AsString, MessageCode.Wire(fakes.Log[i].Code), $"log[{i}]");
                Same(log.AsArray[i]["params"] ?? EmptyObject, (JsonValue)fakes.Log[i].Params, $"log[{i}] params");
                Same(log.AsArray[i]["severity"]!.AsString, EnumWire.Name(fakes.Log[i].Severity), $"log[{i}] severity");
            }
        }
        if (expect["progress"] is { } progress)
        {
            Same(progress.AsArray!.Count, fakes.Progress.Count, "progress length");
            for (int i = 0; i < fakes.Progress.Count; i++)
            {
                Same(progress.AsArray[i].AsArray![0].AsNumber, fakes.Progress[i].AsArray![0].AsNumber, $"progress[{i}] fraction");
                Same(JsonValue.Of(progress.AsArray[i].AsArray!.Skip(1)), JsonValue.Of(fakes.Progress[i].AsArray!.Skip(1)), $"progress[{i}]");
            }
        }
        if (expect["calls"] is { } calls) Same(calls, JsonValue.Of(fakes.Calls), "calls");
        if (expect["stagingAsked"] is { } asked) Same(asked.AsInteger, (long)fakes.StagingAsked, "stagingAsked");
        return true;
    }
}
