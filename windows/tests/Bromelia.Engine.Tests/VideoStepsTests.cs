using System.Globalization;
using Bromelia.Domain;
using Bromelia.Foundation;
using Bromelia.Ports;
using Xunit;
using static Bromelia.Tests.Fixtures;
using CancellationToken = Bromelia.Foundation.CancellationToken;

namespace Bromelia.Engine.Tests;

/// <summary>shared/fixtures/engine/video-steps.cases.json, plan-steps.cases.json, acquire-steps.cases.json and
/// verify-rips.cases.json: each step alone, with fake ports.</summary>
public sealed class VideoStepsTests
{
    static JsonValue EmptyObject => JsonValue.Of(Array.Empty<(string, JsonValue)>());

    static readonly Dictionary<StepKind, IStep> Steps = new IStep[]
        { new AwaitMediaStep(), new ProbeStep(), new ReconcileStep(), new IdentifyStep(), new PlanStep(), new DecideStep(), new AcquireStep(), new ReleaseStep(), new VerifyRipsStep() }
        .ToDictionary(s => s.Kind);

    static JobRequest RequestOf(JsonValue r) => new(
        EnumWire.Parse<JobKind>(r["kind"]?.AsString) ?? JobKind.VideoDisc, r["title"]?.AsString ?? "", r["automatic"]?.AsBool ?? false,
        r["driveId"]?.AsString, r["mediaGeneration"]?.AsInteger, r["sessionId"]?.AsString is { } s ? new Id(s) : null,
        Options: r["options"], Details: r["details"]);

    static BroMessage MessageOf(JsonValue j, Severity severity) =>
        new(MessageCode.Parse(j["code"]!.AsString!)!.Value, (JsonValue.Object)(j["params"] ?? EmptyObject), severity);

    static MakemkvNotice? NoticeOf(string? name) => name switch
    {
        "keyExpired" => new MakemkvNotice.KeyExpired(),
        "evaluationNotStarted" => new MakemkvNotice.EvaluationNotStarted(),
        "versionTooOld" => new MakemkvNotice.VersionTooOld(),
        "libreDriveRequired" => new MakemkvNotice.LibreDriveRequired(),
        _ => null,
    };

    static string? NoticeName(MakemkvNotice? n) => n switch
    {
        MakemkvNotice.KeyExpired => "keyExpired",
        MakemkvNotice.EvaluationNotStarted => "evaluationNotStarted",
        MakemkvNotice.VersionTooOld => "versionTooOld",
        MakemkvNotice.LibreDriveRequired => "libreDriveRequired",
        MakemkvNotice.LibreDrive => "libreDrive",
        _ => null,
    };

    /// <summary>The ports a case gives, recording every call.</summary>
    sealed class Fakes : IMakemkv, IMatroska, IDriveControl, IDriveLookup, IMakemkvSetup, IProgressReporter, IJobLogger, ILibraryPort, ICatalogQueries,
        IDecisionPort, ISpaceReservations, ICheckpointer
    {
        readonly JsonValue _given;
        public readonly List<string> Calls = new();
        public readonly List<BroMessage> Log = new();
        public readonly List<JsonValue> Progress = new();
        /// <summary>Probe's fingerprint ('archivedBefore probe').</summary>
        public string? Fingerprint;
        public DecisionDto? Question;
        public MemoryFileSystem Fs = null!;
        public CancellationSource Cancel = null!;
        /// <summary>makemkv.runs not taken yet.</summary>
        readonly Queue<JsonValue> _runs;

        public Fakes(JsonValue given)
        {
            _given = given;
            _runs = new Queue<JsonValue>(given["makemkv"]?["runs"]?.AsArray ?? new List<JsonValue>());
        }

        /// <summary>A fingerprint "probe" stands for Probe's, both ways.</summary>
        JsonValue Probed(JsonValue checkpoint, bool toProbe) => checkpoint is JsonValue.Object o
            ? new JsonValue.Object(o.Members.Select(m => m.Key == "fingerprint" && m.Value.AsString is { } f
                && (toProbe ? f == Fingerprint : f == "probe") ? new KeyValuePair<string, JsonValue>(m.Key, JsonValue.Of(toProbe ? "probe" : Fingerprint)) : m).ToList())
            : checkpoint;

        public JsonValue? Saved => _given["checkpoint"] is { } c ? Probed(c, false) : null;

        public Task Save(JsonValue checkpoint)
        {
            lock (Calls) Calls.Add("checkpoint " + OneLine(Probed(checkpoint, true)));
            return Task.CompletedTask;
        }

        static string Describe(MakemkvSource source) => source switch
        {
            MakemkvSource.Drive d => "disc:" + d.Index.ToString(CultureInfo.InvariantCulture),
            MakemkvSource.Iso i => "iso:" + i.Path,
            MakemkvSource.File f => "file:" + f.Path,
            _ => "?",
        };

        JsonValue NextRun()
        {
            lock (_runs)
                return _runs.Count > 0 ? _runs.Dequeue()
                    : throw new BroFailure(new BroError(MessageCode.Wire(MessageCode.InternalUnexpected), ("detail", JsonValue.Of("no makemkv run left"))));
        }

        /// <summary>What a scripted run ends with: its outcome and what it reported.</summary>
        MakemkvRun Ran(JsonValue run, IRunSink sink, IReadOnlyList<string> produced)
        {
            foreach (var line in run["robot"]?.AsArray ?? new List<JsonValue>())
                if (Robot.ParseLine(line.AsString!) is { } e) sink.Event(e);
            if (run["cancel"]?.AsBool == true) Cancel.Cancel();
            var readErrors = (run["readErrors"]?.AsArray ?? new List<JsonValue>())
                .Select(t => new RobotMessage(2003, 0, 0, t.AsString!, t.AsString!, new List<string>())).ToList();
            // MakeMKV's debug log: the run names it, and it holds the text (written by the "process", so not recorded).
            if (run["debugLog"] is { } debug && debug["text"]?.AsString is { } debugText)
                Fs.Put(debug["path"]!.AsString!, System.Text.Encoding.UTF8.GetBytes(debugText));
            var outcome = new RunOutcome(null, null, (int)(run["exitCode"]?.AsInteger ?? 0), null, null, readErrors, null, run["debugLog"]?["path"]?.AsString,
                EnumWire.Parse<StatusWord>(run["status"]?.AsString) ?? StatusWord.Success, run["error"] is { } err ? MessageOf(err, Severity.Error) : null,
                (run["produced"]?.AsArray?.Select(p => p.AsString!).ToList() ?? produced.ToList()), run["abandoned"]?.AsBool ?? false);
            BroMessage? Problem(string key) => run[key] is { } p ? MessageOf(p, Severity.Warning) : null;
            return new MakemkvRun(outcome, NoticeOf(run["notice"]?.AsString), null, null, Problem("transcriptProblem"), Problem("settingsProblem"),
                Problem("isolationWarning"));
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
            Fs.PutFolder(path);
            return path;
        }

        public Task<CommitResult> Commit(UnitRecord unit, CommitIntent intent, IReadOnlyList<UnitFile> files, CancellationToken cancel) => throw new NotSupportedException();

        public Task<CommitResult> Quarantine(UnitRecord unit, CommitIntent intent, IReadOnlyList<UnitFile> files, string tag, CancellationToken cancel) =>
            throw new NotSupportedException();

        public Task<IReadOnlyList<UnitRecord>> ArchivedBefore(string fingerprint)
        {
            lock (Calls) Calls.Add("archivedBefore " + (fingerprint == Fingerprint ? "probe" : fingerprint));
            if (fingerprint != Fingerprint) return Task.FromResult<IReadOnlyList<UnitRecord>>(new List<UnitRecord>());
            var units = (_given["catalog"]?.AsArray ?? new List<JsonValue>()).Select(u => new UnitRecord(new Id(u["id"]!.AsString!),
                u["libraryId"]!.AsString!, null, null, u["path"]!.AsString!, "", 3, UnitState.Committed,
                EnumWire.Parse<UnitStatus>(u["status"]!.AsString) ?? UnitStatus.Success, "", MediaKind.Movie, "", "", false, fingerprint, "", null,
                null, null, null, "", 0, 0, 1, Instant.Parse("2026-09-01T00:00:00.000Z")!.Value,
                Instant.Parse(u["committedAt"]!.AsString!)!.Value)).ToList();
            return Task.FromResult<IReadOnlyList<UnitRecord>>(units);
        }

        public Task<string> Ask(DecisionDto decision)
        {
            lock (Calls)
            {
                Question = decision;
                Calls.Add($"ask {decision.Id} {MessageCode.Wire(decision.Question.Code)} {string.Join(",", decision.Answers.Select(a => a.Id))}");
            }
            return _given["answer"]?.AsString is { } answer ? Task.FromResult(answer)
                : Task.FromException<string>(new BroFailure(new BroError(MessageCode.Wire(MessageCode.JobCancelled))));
        }

        public Task Reserve(string volume, string path, long need, long free)
        {
            lock (Calls) Calls.Add(FormattableString.Invariant($"reserve {volume} {path} {need} {free}"));
            if (_given["reserveFails"] is { } e) throw new BroFailure(new BroError(e["code"]!.AsString!, (JsonValue.Object)(e["params"] ?? EmptyObject)));
            return Task.CompletedTask;
        }

        public DriveLocation? Find(string driveId)
        {
            if (_given["drives"]?[driveId] is not { } d) return null;
            return new DriveLocation((int?)d["makemkvIndex"]?.AsInteger, d["device"]?.AsString ?? "", d["configId"]?.AsString,
                d["generation"]?.AsInteger, d["label"]?.AsString ?? "", d["flags"]?.AsInteger is { } f ? new DiscFlags((int)f) : null);
        }

        public MakemkvInvocation Invocation(Config config, EffectiveProfile profile, string? selectionOverride, string jobDirectory)
        {
            lock (Calls)
                Calls.Add($"invocation {selectionOverride ?? "-"} minLength={profile.Profile["makemkv"]?["minLengthSeconds"]?.AsInteger?.ToString(CultureInfo.InvariantCulture) ?? "null"}");
            return new MakemkvInvocation(new MakemkvRunSettings(new Dictionary<string, string>(), null, "", jobDirectory), new MakemkvOptions());
        }

        public Task<ListingRun> Listing(MakemkvSource source, MakemkvInvocation invocation, IRunSink sink, CancellationToken cancel)
        {
            lock (Calls) Calls.Add("listing " + Describe(source));
            if (_given["makemkv"]?["runs"] != null)
            {
                var run = NextRun();
                var listed = new ListingBuilder();
                if (run["listing"] is { } disc)
                    foreach (var line in GeneratedListing(disc).Split('\n'))
                        if (Robot.ParseLine(line) is { } e)
                        {
                            sink.Event(e);
                            listed.Feed(e);
                        }
                return Task.FromResult(new ListingRun(listed.Build(), Ran(run, sink, Array.Empty<string>())));
            }
            var m = _given["makemkv"] ?? EmptyObject;
            var text = m["listing"] is { } l ? GeneratedListing(l) : string.Join("\n", (m["robot"]?.AsArray ?? new List<JsonValue>()).Select(x => x.AsString));
            var builder = new ListingBuilder();
            foreach (var line in text.Split('\n'))
                if (Robot.ParseLine(line) is { } e)
                {
                    sink.Event(e);
                    builder.Feed(e);
                }
            var o = m["outcome"] ?? EmptyObject;
            var outcome = new RunOutcome(null, null, (int)(o["exitCode"]?.AsInteger ?? 0), null, null, new List<RobotMessage>(), null, null,
                EnumWire.Parse<StatusWord>(o["status"]?.AsString) ?? StatusWord.Success, o["error"] is { } err ? MessageOf(err, Severity.Error) : null,
                new List<string>());
            BroMessage? Problem(string key) => m[key] is { } p ? MessageOf(p, Severity.Warning) : null;
            return Task.FromResult(new ListingRun(builder.Build(),
                new MakemkvRun(outcome, NoticeOf(m["notice"]?.AsString), m["libreDrive"]?.AsString, m["version"]?.AsString, Problem("transcriptProblem"), Problem("settingsProblem"),
                    Problem("isolationWarning"))));
        }

        public Task<IReadOnlyList<MakemkvDrive>> ScanDrives(CancellationToken cancel) => throw new NotSupportedException();

        /// <summary>given.matroska: a probe by file name ({duration, tracks, chapters} or null), missing, cancelled.</summary>
        public Task<MkvProbe?> Probe(string file, CancellationToken cancel)
        {
            lock (Calls) Calls.Add("probe " + file);
            var m = _given["matroska"] ?? EmptyObject;
            if (m["missing"]?.AsBool == true)
                throw new BroFailure(new BroError(MessageCode.Wire(MessageCode.ToolMissing), ("tool", JsonValue.Of("mkvmerge"))));
            if (m["cancelled"]?.AsBool == true) throw new BroFailure(new BroError(MessageCode.Wire(MessageCode.JobCancelled)));
            if (m[file[(file.LastIndexOf('/') + 1)..]] is not JsonValue.Object p) return Task.FromResult<MkvProbe?>(null);
            return Task.FromResult<MkvProbe?>(new MkvProbe(p["duration"]?.AsNumber,
                (p["tracks"]?.AsArray ?? new List<JsonValue>()).Select((t, i) => new MkvTrack(i, t.AsString!)).ToList(), (int)(p["chapters"]?.AsInteger ?? 0)));
        }

        public Task<bool> Remux(IReadOnlyList<string> arguments, CancellationToken cancel) => throw new NotSupportedException();

        public Task<IReadOnlyList<string>?> Split(IReadOnlyList<int> chapters, string input, CancellationToken cancel) => throw new NotSupportedException();

        public Task<IReadOnlyList<Duration>?> ChapterTimes(string file, CancellationToken cancel) => throw new NotSupportedException();

        public Task<MakemkvRun> Rip(MakemkvSource source, string title, string destination, MakemkvInvocation invocation, IRunSink sink,
            CancellationToken cancel)
        {
            lock (Calls) Calls.Add($"rip {Describe(source)} {title} {destination}");
            var run = NextRun();
            var files = (run["files"]?.AsArray ?? new List<JsonValue>()).Select(f => f.AsString!).ToList();
            foreach (var f in files) Fs.Put(destination + "/" + f, Array.Empty<byte>());
            return Task.FromResult(Ran(run, sink, files));
        }

        public Task<MakemkvRun> Backup(MakemkvSource source, bool decrypt, string destination, MakemkvInvocation invocation, IRunSink sink,
            CancellationToken cancel)
        {
            lock (Calls) Calls.Add($"backup {Describe(source)} {(decrypt ? "true" : "false")} {destination}");
            var run = NextRun();
            var name = destination[(destination.LastIndexOf('/') + 1)..];
            List<string> produced;
            if (run["image"]?.AsBool == true)
            {
                Fs.Put(destination, new byte[32769].Concat(System.Text.Encoding.ASCII.GetBytes(run["header"]?.AsString ?? "")).ToArray());
                produced = new List<string> { name };
            }
            else
            {
                var files = (run["files"]?.AsArray ?? new List<JsonValue>()).Select(f => f.AsString!).ToList();
                Fs.PutFolder(destination);
                foreach (var f in files) Fs.Put(destination + "/" + f, Array.Empty<byte>());
                produced = files.Select(f => f.Split('/')[0]).Distinct().OrderBy(f => f, StringComparer.Ordinal).ToList();
            }
            return Task.FromResult(Ran(run, sink, produced));
        }

        public Task Eject(string device)
        {
            lock (Calls) Calls.Add("eject " + device);
            if (_given["eject"] is { } e) throw new BroFailure(new BroError(e["code"]!.AsString!, (JsonValue.Object)(e["params"] ?? EmptyObject)));
            return Task.CompletedTask;
        }

        public Task CloseTray(string device) => throw new NotSupportedException();

        public Task<string?> WaitForMount(string device, Duration timeout, CancellationToken cancel)
        {
            lock (Calls) Calls.Add(FormattableString.Invariant($"waitForMount {device} {timeout.Seconds}"));
            return Task.FromResult(_given["mount"]?.AsString);
        }

        public DiscContent ProbeContent(string device) => throw new NotSupportedException();

        public ISectorReader OpenRaw(string device) => throw new NotSupportedException();

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
    }

    /// <summary>Every key of <paramref name="expected"/> is in <paramref name="actual"/> with the same value; arrays have the
    /// same length and match element by element.</summary>
    static void Subset(JsonValue expected, JsonValue? actual, string path)
    {
        if (expected is JsonValue.Object o)
        {
            if (actual is not JsonValue.Object) throw new Xunit.Sdk.XunitException($"{path}: expected an object, got {actual}");
            foreach (var (k, v) in o.Members) Subset(v, actual[k], path + "." + k);
        }
        else if (expected is JsonValue.Array a)
        {
            if (actual?.AsArray is not { } b || b.Count != a.Items.Count)
                throw new Xunit.Sdk.XunitException($"{path}: expected {expected.ToString().TrimEnd()}, got {actual?.ToString().TrimEnd() ?? "nothing"}");
            for (int i = 0; i < a.Items.Count; i++) Subset(a.Items[i], b[i], $"{path}[{i}]");
        }
        else if (expected.AsNumber is { } n && actual?.AsNumber is { } m) Same(n, m, path);
        else Same(expected, actual ?? JsonValue.Null.Instance, path);
    }

    static (StepReport? Report, BroError? Thrown) Run(IStep step, StepContext context)
    {
        try
        {
            return (Task.Run(() => step.Run(context)).GetAwaiter().GetResult(), null);
        }
        catch (BroFailure f)
        {
            return (null, f.Error);
        }
    }

    [Fact]
    public void TheSharedCasesPass() => RunCases("engine/video-steps.cases.json", Case);

    [Fact]
    public void ThePlanCasesPass() => RunCases("engine/plan-steps.cases.json", Case);

    [Fact]
    public void TheAcquireCasesPass() => RunCases("engine/acquire-steps.cases.json", Case);

    [Fact]
    public void TheVerifyRipsCasesPass() => RunCases("engine/verify-rips.cases.json", Case);

    static bool Case(string id, JsonValue given, JsonValue expect)
    {
        var step = Steps[EnumWire.Parse<StepKind>(given["step"]!.AsString)!.Value];
        var fakes = new Fakes(given);
        var config = ConfigCodec.Decode(JsonValue.EncodeCanonical(given["config"] ?? JsonValue.Of(("version", JsonValue.Of(3L)))));
        var outputs = new List<StepOutput>();
        if (given["probe"] is { } probe)
        {
            var listing = probe["disc"] is { } disc ? Listing(GeneratedListing(disc)) : null;
            fakes.Fingerprint = listing != null ? Bromelia.Domain.Fingerprint.Of(listing) : null;
            outputs.Add(new StepOutput(StepKind.Probe, ProbeOutput.Encode(new ProbeOutput(listing, fakes.Fingerprint, null, false))));
        }
        outputs.AddRange((given["outputs"]?.AsArray ?? new List<JsonValue>())
            .Select(o => new StepOutput(EnumWire.Parse<StepKind>(o["kind"]!.AsString)!.Value, o["data"]!, null, (int)(o["readErrors"]?.AsInteger ?? 0))));
        var fs = new MemoryFileSystem();
        fakes.Fs = fs;
        var jobId = new Id("00000000-0000-4000-8000-000000000001");
        var staging = (given["libraries"]?["lib"]?["path"]?.AsString ?? "/lib") + "/.bromelia/staging/" + jobId.Value;
        foreach (var t in given["tree"]?.AsArray ?? new List<JsonValue>())
        {
            if (t.AsString!.EndsWith('/')) fs.PutFolder(staging + "/" + t.AsString!.TrimEnd('/'));
            else fs.Put(staging + "/" + t.AsString, Array.Empty<byte>());
        }
        foreach (var (name, header) in given["images"]?.AsObject ?? new List<KeyValuePair<string, JsonValue>>())
            fs.Put(staging + "/" + name, new byte[32769].Concat(System.Text.Encoding.ASCII.GetBytes(header.AsString!)).ToArray());
        foreach (var f in given["files"]?.AsArray ?? new List<JsonValue>()) fs.CreateDirectory(f.AsString!, parentsMustExist: false);
        foreach (var (path, v) in given["volumes"]?.AsObject ?? new List<KeyValuePair<string, JsonValue>>())
            fs.Volumes[path] = new VolumeInfo(v["id"]!.AsString!, v["free"]!.AsInteger!.Value, "apfs", false);
        Decided? finishing = given["finishing"] is { } end
            ? new Decided(EnumWire.Parse<Outcome>(end["outcome"]!.AsString)!.Value, end["error"] is { } why ? new BroError(why["code"]!.AsString!) : null)
            : null;
        var request = RequestOf(given["request"]!);
        var cancel = new CancellationSource();
        fakes.Cancel = cancel;
        StepContext ContextOf(JobSnapshot job) => new(job, cancel.Token, fakes, fakes, fakes, new ToolSet(fakes, fakes), fs, null!, fakes, fakes, fakes,
            config, fakes, fakes, fakes, fakes, "/data/jobs/" + job.Id.Value, new RecoveryServiceTests.Renderer(), "test", finishing);
        if (given["identify"]?.AsBool == true)
        {
            var identified = Run(Steps[StepKind.Identify], ContextOf(new JobSnapshot(jobId, request, null, outputs.ToList())));
            outputs.Add(new StepOutput(StepKind.Identify, identified.Report!.Data));
            fakes.Log.Clear();
            fakes.Calls.Clear();
            fakes.Progress.Clear();
        }
        var job = new JobSnapshot(jobId, request, null, outputs);
        var context = ContextOf(job);

        if (expect["resources"] is { } resources)
        {
            Same(resources["drives"], JsonValue.Of(step.Resources(job).Drives.Select(d => JsonValue.Of(d))), "resources.drives");
            if (resources["libraryWrites"] is { } writes)
                Same(writes, JsonValue.Of(step.Resources(job).LibraryWrites.Select(d => JsonValue.Of(d))), "resources.libraryWrites");
            if (resources["acquisition"]?.AsBool is { } acquisition) Same(acquisition, step.Resources(job).Acquisition, "resources.acquisition");
        }
        if (expect["resumes"] is { } resumes)
        {
            Same(resumes["resumability"]!.AsString, EnumWire.Name(step.Resumability), "resumability");
            Same(resumes["checkpoint"]?.AsString, step.ResumeCheckpoint, "resumeCheckpoint");
        }
        fs.Recording = true;
        var (report, thrown) = Run(step, context);
        fs.Recording = false;
        var error = thrown ?? report?.Error;
        if (expect["error"] is { } expectedError)
        {
            Same(expectedError["code"]!.AsString, error?.Code, "error");
            Same(expectedError["params"] ?? EmptyObject, (JsonValue?)error?.Params ?? EmptyObject, "error params");
        }
        else if (error != null) throw new Xunit.Sdk.XunitException($"unexpected error {error.Code} {error.Params}");
        if (expect["notice"] is { } notice) Same(notice.AsString, NoticeName(report?.Notice), "notice");
        Same(expect["skipped"]?.AsBool ?? false, report?.Skipped ?? false, "skipped");
        if (expect["data"] is { } data) Subset(data, report?.Data, "data");
        if (expect["log"] is { } log)
        {
            var got = fakes.Log;
            Same(log.AsArray!.Count, got.Count, "log length (" + string.Join(", ", got.Select(m => MessageCode.Wire(m.Code))) + ")");
            for (int i = 0; i < got.Count; i++)
            {
                var e = log.AsArray[i];
                Same(e.AsString ?? e["code"]!.AsString, MessageCode.Wire(got[i].Code), $"log[{i}]");
                if (e is JsonValue.Object)
                {
                    Same(e["params"] ?? EmptyObject, (JsonValue)got[i].Params, $"log[{i}] params");
                    if (e["severity"]?.AsString is { } sev) Same(sev, EnumWire.Name(got[i].Severity), $"log[{i}] severity");
                }
            }
        }
        if (expect["calls"] is { } calls) Same(string.Join(" | ", calls.AsArray!.Select(c => c.AsString)), string.Join(" | ", fakes.Calls), "calls");
        if (expect["progress"] is { } progress) Subset(progress, JsonValue.Of(fakes.Progress), "progress");
        if (expect["skipJob"] is { } skipJob)
        {
            Same(skipJob is JsonValue.Null ? JsonValue.Null.Instance : skipJob["code"]!, JsonValue.Of(report?.SkipJob is { } sj ? MessageCode.Wire(sj.Code) : null), "skipJob");
            if (skipJob["params"] is { } sp) Same(sp, (JsonValue?)report?.SkipJob?.Params ?? EmptyObject, "skipJob params");
        }
        if (expect["appendSteps"] is { } append)
            Same(append, JsonValue.Of((report?.AppendSteps ?? new List<StepKind>()).Select(k => JsonValue.Of(EnumWire.Name(k)))), "appendSteps");
        if (expect["plan"] is { } plan)
            Subset(plan, report?.Plan is { } p ? JsonValue.Of(("mode", JsonValue.Of(p.Mode)), ("profileId", JsonValue.Of(p.ProfileId)),
                ("libraryId", JsonValue.Of(p.LibraryId))) : null, "plan");
        if (expect["readErrors"]?.AsInteger is { } readErrors) Same(readErrors, (long)(report?.ReadErrors ?? 0), "readErrors");
        if (expect["tree"] is { } tree)
            Same(tree, JsonValue.Of((fs.Exists(staging) ? FileTree.Files(fs, staging) : new List<string>()).Select(f => JsonValue.Of(f))), "tree");
        if (expect["fs"] is { } ops) Same(string.Join(" | ", ops.AsArray!.Select(o => o.AsString)), string.Join(" | ", fs.Ops), "fs");
        foreach (var (name, text) in expect["jobFiles"]?.AsObject ?? new List<KeyValuePair<string, JsonValue>>())
        {
            var path = "/data/jobs/" + jobId.Value + "/" + name;
            Same(text, fs.Exists(path) ? JsonValue.Of(System.Text.Encoding.UTF8.GetString(fs.Read(path))) : JsonValue.Null.Instance, "job file " + name);
        }
        foreach (var (name, wanted) in expect["json"]?.AsObject ?? new List<KeyValuePair<string, JsonValue>>())
        {
            var want = wanted.AsString == "probe" ? Bromelia.Domain.Listing.Encode(Listing(GeneratedListing(given["probe"]!["disc"]!))) : wanted;
            Same(want, JsonValue.Parse(fs.Read(staging + "/" + name)), "json " + name);
        }
        if (expect["question"] is { } question)
        {
            Same(question["code"], JsonValue.Of(fakes.Question is { } q ? MessageCode.Wire(q.Question.Code) : null), "question");
            Same(question["params"] ?? EmptyObject, (JsonValue?)fakes.Question?.Question.Params ?? EmptyObject, "question params");
        }
        return true;
    }

    /// <summary>The canonical encoding on one line, as JobLog writes it.</summary>
    static string OneLine(JsonValue value)
    {
        var canonical = value.ToString();
        var sb = new System.Text.StringBuilder();
        for (int i = 0; i < canonical.Length; i++)
        {
            if (canonical[i] != '\n')
            {
                sb.Append(canonical[i]);
                continue;
            }
            while (i + 1 < canonical.Length && canonical[i + 1] == ' ') i++;
        }
        return sb.ToString();
    }

    static Listing Listing(string robot)
    {
        var builder = new ListingBuilder();
        foreach (var line in robot.Split('\n'))
            if (Robot.ParseLine(line) is { } e) builder.Feed(e);
        return builder.Build();
    }
}
