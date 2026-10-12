using System.Globalization;
using Bromelia.Domain;
using Bromelia.Foundation;
using Bromelia.Ports;
using Xunit;
using static Bromelia.Tests.Fixtures;
using CancellationToken = Bromelia.Foundation.CancellationToken;

namespace Bromelia.Engine.Tests;

/// <summary>shared/fixtures/engine/video-steps.cases.json: each step alone, with fake ports.</summary>
public sealed class VideoStepsTests
{
    static JsonValue EmptyObject => JsonValue.Of(Array.Empty<(string, JsonValue)>());

    static readonly Dictionary<StepKind, IStep> Steps = new IStep[]
        { new AwaitMediaStep(), new ProbeStep(), new ReconcileStep(), new IdentifyStep(), new ReleaseStep() }.ToDictionary(s => s.Kind);

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
    sealed class Fakes : IMakemkv, IDriveControl, IDriveLookup, IMakemkvSetup, IProgressReporter, IJobLogger
    {
        readonly JsonValue _given;
        public readonly List<string> Calls = new();
        public readonly List<BroMessage> Log = new();
        public readonly List<JsonValue> Progress = new();

        public Fakes(JsonValue given) => _given = given;

        public DriveLocation? Find(string driveId)
        {
            if (_given["drives"]?[driveId] is not { } d) return null;
            return new DriveLocation((int?)d["makemkvIndex"]?.AsInteger, d["device"]?.AsString ?? "", d["configId"]?.AsString,
                d["generation"]?.AsInteger, d["label"]?.AsString ?? "", d["flags"]?.AsInteger is { } f ? new DiscFlags((int)f) : null);
        }

        public MakemkvInvocation Invocation(EffectiveProfile profile, string? selectionOverride, string jobDirectory)
        {
            lock (Calls)
                Calls.Add($"invocation {selectionOverride ?? "-"} minLength={profile.Profile["makemkv"]?["minLengthSeconds"]?.AsInteger?.ToString(CultureInfo.InvariantCulture) ?? "null"}");
            return new MakemkvInvocation(new MakemkvRunSettings(new Dictionary<string, string>(), null, "", jobDirectory), new MakemkvOptions());
        }

        public Task<ListingRun> Listing(MakemkvSource source, MakemkvInvocation invocation, IRunSink sink, CancellationToken cancel)
        {
            lock (Calls)
                Calls.Add("listing " + source switch
                {
                    MakemkvSource.Drive d => "disc:" + d.Index.ToString(CultureInfo.InvariantCulture),
                    MakemkvSource.Iso i => "iso:" + i.Path,
                    MakemkvSource.File f => "file:" + f.Path,
                    _ => "?",
                });
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
                new MakemkvRun(outcome, NoticeOf(m["notice"]?.AsString), m["libreDrive"]?.AsString, null, Problem("transcriptProblem"), Problem("settingsProblem"))));
        }

        public Task<IReadOnlyList<MakemkvDrive>> ScanDrives(CancellationToken cancel) => throw new NotSupportedException();

        public Task<MakemkvRun> Rip(MakemkvSource source, string title, string destination, MakemkvInvocation invocation, IRunSink sink,
            CancellationToken cancel) => throw new NotSupportedException();

        public Task<MakemkvRun> Backup(MakemkvSource source, bool decrypt, string destination, MakemkvInvocation invocation, IRunSink sink,
            CancellationToken cancel) => throw new NotSupportedException();

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
    public void TheSharedCasesPass() => RunCases("engine/video-steps.cases.json", (id, given, expect) =>
    {
        var step = Steps[EnumWire.Parse<StepKind>(given["step"]!.AsString)!.Value];
        var fakes = new Fakes(given);
        var config = ConfigCodec.Decode(JsonValue.EncodeCanonical(given["config"] ?? JsonValue.Of(("version", JsonValue.Of(3L)))));
        var outputs = (given["outputs"]?.AsArray ?? new List<JsonValue>())
            .Select(o => new StepOutput(EnumWire.Parse<StepKind>(o["kind"]!.AsString)!.Value, o["data"]!)).ToList();
        var job = new JobSnapshot(new Id("00000000-0000-4000-8000-000000000001"), RequestOf(given["request"]!), null, outputs);
        var cancel = new CancellationSource();
        var context = new StepContext(job, cancel.Token, fakes, fakes, null!, new ToolSet(fakes, null!), null!, null!, fakes, null!, null!, config,
            fakes, fakes, "/data/jobs/" + job.Id.Value);

        if (expect["resources"] is { } resources)
            Same(resources["drives"], JsonValue.Of(step.Resources(job).Drives.Select(d => JsonValue.Of(d))), "resources.drives");
        var (report, thrown) = Run(step, context);
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
        return true;
    });
}
