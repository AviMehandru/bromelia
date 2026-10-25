using System.Text;
using Bromelia.Domain;
using Bromelia.Foundation;
using Bromelia.Ports;
using Xunit;
using static Bromelia.Tests.Fixtures;
using CancellationToken = Bromelia.Foundation.CancellationToken;

namespace Bromelia.Engine.Tests;

/// <summary>shared/fixtures/engine/name-steps.cases.json: NameStep alone, on one file tree in memory holding the library and
/// the job's staging folder, a clock whose local time is UTC plus an offset, a checkpointer kept between attempts, a drive
/// lookup and the configuration. The format is in claudeDocs/tools/rebuild-phase3/gen-name-cases.py.</summary>
public sealed class NameStepTests
{
    const string JobId = "00000000-0000-4000-8000-000000000001";
    static JsonValue.Object EmptyObject => new(new List<KeyValuePair<string, JsonValue>>());

    sealed class Fakes : IJobLogger, ILibraryPort, IClock, ICheckpointer, IDriveLookup
    {
        readonly JsonValue _given;
        public readonly MemoryFileSystem Fs;
        public readonly List<BroMessage> Log = new();
        public JsonValue? Drives;
        /// <summary>What the checkpointer holds: an attempt sees what the one before it saved.</summary>
        public JsonValue? Stored;
        public int Saves;

        public Fakes(JsonValue given, MemoryFileSystem fs)
        {
            _given = given;
            Fs = fs;
            Drives = given["drives"];
            Stored = given["checkpoint"];
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
            Fs.PutFolder(path);
            return path;
        }

        public Task<CommitResult> Commit(UnitRecord unit, CommitIntent intent, IReadOnlyList<UnitFile> files, CancellationToken cancel) => throw new NotSupportedException();

        public Task<CommitResult> Quarantine(UnitRecord unit, CommitIntent intent, IReadOnlyList<UnitFile> files, string tag, CancellationToken cancel) =>
            throw new NotSupportedException();

        public Instant Now() => Instant.Parse(_given["now"]!.AsString!)!.Value;

        public Duration Monotonic() => throw new NotSupportedException();

        public Task Sleep(Duration duration, CancellationToken cancel) => throw new NotSupportedException();

        public ITimerHandle Timer(TimerSchedule schedule, Action handler) => throw new NotSupportedException();

        /// <summary>UTC plus the case's utcOffsetMinutes.</summary>
        public LocalTime LocalTime(Instant instant)
        {
            var t = DateTimeOffset.FromUnixTimeMilliseconds(instant.UnixMilliseconds)
                .ToOffset(TimeSpan.FromMinutes(_given["utcOffsetMinutes"]?.AsInteger ?? 0));
            return new LocalTime(t.Year, t.Month, t.Day, t.Hour, t.Minute, t.Second);
        }

        /// <summary>Fixed for the attempt, as the runner's is.</summary>
        public JsonValue? Saved { get; set; }

        public Task Save(JsonValue checkpoint)
        {
            Stored = checkpoint;
            Saves++;
            return Task.CompletedTask;
        }

        public DriveLocation? Find(string driveId) =>
            Drives?[driveId] is { } d ? new DriveLocation(null, "", d["configId"]?.AsString, null) : null;
    }

    static JobRequest RequestOf(JsonValue r) => new(
        EnumWire.Parse<JobKind>(r["kind"]?.AsString) ?? JobKind.VideoDisc, r["title"]?.AsString ?? "", r["automatic"]?.AsBool ?? false,
        r["driveId"]?.AsString, r["mediaGeneration"]?.AsInteger, null, Options: r["options"], Details: r["details"]);

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

    static void SameError(JsonValue expected, BroError? error, string what)
    {
        Same(expected["code"]!.AsString, error?.Code, what);
        Same(expected["params"] ?? EmptyObject, (JsonValue?)error?.Params ?? EmptyObject, what + " params");
    }

    [Fact]
    public void TheNameCasesPass() => RunCases("engine/name-steps.cases.json", Case);

    static bool Case(string id, JsonValue given, JsonValue expect)
    {
        var step = new NameStep();
        var fs = new MemoryFileSystem();
        var fakes = new Fakes(given, fs);
        var library = given["libraries"]?["lib"]?["path"]?.AsString ?? "/lib";
        var staging = library + "/.bromelia/staging/" + JobId;
        void Put(string root, JsonValue? entries)
        {
            foreach (var (rel, content) in entries?.AsObject ?? new List<KeyValuePair<string, JsonValue>>())
                if (rel.EndsWith('/')) fs.PutFolder(root + "/" + rel.TrimEnd('/')); // an empty folder
                else fs.Put(root + "/" + rel, Encoding.UTF8.GetBytes(content.AsString!));
        }
        Put(library, given["library"]);
        Put(staging, given["staging"]);
        foreach (var f in given["fails"]?.AsArray ?? new List<JsonValue>())
            fs.Fails.Add((f["op"]!.AsString!, f["path"]!.AsString!, new BroError(f["error"]!["code"]!.AsString!,
                (JsonValue.Object)(f["error"]!["params"] ?? EmptyObject))));
        var cancel = new CancellationSource();
        if (given["cancelAt"] is { } start && start["op"]?.AsString == "start") cancel.Cancel(); // before the step runs
        else if (given["cancelAt"] is { } at)
            fs.OnOp = (op, path) =>
            {
                if (op == at["op"]!.AsString && path == at["path"]!.AsString) cancel.Cancel();
            };
        var outputs = (given["outputs"]?.AsArray ?? new List<JsonValue>())
            .Select(o => new StepOutput(EnumWire.Parse<StepKind>(o["kind"]!.AsString)!.Value, o["data"]!, null, 0)).ToList();
        var job = new JobSnapshot(new Id(JobId), RequestOf(given["request"]!), null, outputs,
            given["startedAt"]?.AsString is { } started ? Instant.Parse(started) : null);
        var config = new Config(given["config"] ?? JsonValue.Of(), new List<Issue>());

        if (expect["resources"] is { } resources)
            Same(resources["libraryWrites"], JsonValue.Of(step.Resources(job).LibraryWrites.Select(d => JsonValue.Of(d))), "resources.libraryWrites");
        if (expect["resumes"] is { } resumes) Same(resumes["resumability"]!.AsString, EnumWire.Name(step.Resumability), "resumability");

        int attempts = (int)(given["attempts"]?.AsInteger ?? 1);
        StepReport? report = null;
        BroError? thrown = null;
        for (int attempt = 1; attempt <= attempts; attempt++)
        {
            if (attempt > 1)
            {
                fs.Fails.Clear();
                fs.OnOp = null;
                cancel = new CancellationSource();
            }
            if (attempt == attempts)
            {
                if (attempt > 1 && given["between"] is { } between)
                {
                    Put(library, between["library"]);
                    Put(staging, between["staging"]);
                    if (between["drives"] is { } drives) fakes.Drives = drives;
                }
                fs.Ops.Clear();
                fakes.Log.Clear();
                fakes.Saves = 0;
                fs.Recording = true;
            }
            fakes.Saved = fakes.Stored;
            var context = new StepContext(job, cancel.Token, null!, fakes, null!, new ToolSet(null!, null!), fs, fakes, null!, fakes, null!, config,
                fakes, null!, null!, fakes, "/data/jobs/" + JobId, null!, "test");
            (report, thrown) = Run(step, context);
            fs.Recording = false;
            if (attempt == 1 && attempts > 1)
            {
                var first = thrown ?? report?.Error;
                if (expect["firstError"] is { } fe) SameError(fe, first, "the first attempt's error");
                else if (first != null) throw new Xunit.Sdk.XunitException($"the first attempt failed: {first.Code} {first.Params}");
            }
        }

        var error = thrown ?? report?.Error;
        if (expect["error"] is { } expectedError) SameError(expectedError, error, "error");
        else if (error != null) throw new Xunit.Sdk.XunitException($"unexpected error {error.Code} {error.Params}");
        Same(expect["skipped"]?.AsBool ?? false, report?.Skipped ?? false, "skipped");
        if (expect["data"] is { } data) Same(data, report?.Data ?? JsonValue.Null.Instance, "data");
        if (expect["log"] is { } log)
        {
            var got = fakes.Log;
            Same(log.AsArray!.Count, got.Count, "log length (" + string.Join(", ", got.Select(m => MessageCode.Wire(m.Code))) + ")");
            for (int i = 0; i < got.Count; i++)
            {
                Same(log.AsArray[i]["code"]!.AsString, MessageCode.Wire(got[i].Code), $"log[{i}]");
                Same(log.AsArray[i]["params"] ?? EmptyObject, (JsonValue)got[i].Params, $"log[{i}] params");
            }
        }
        if (expect["fs"] is { } ops) Same(string.Join("\n", ops.AsArray!.Select(o => o.AsString)), string.Join("\n", fs.Ops), "fs");
        if (expect["tree"] is { } tree)
            Same(tree, JsonValue.Of((fs.Exists(staging) ? FileTree.Files(fs, staging) : new List<string>()).Select(f => JsonValue.Of(f))), "tree");
        if (expect.AsObject!.Any(m => m.Key == "checkpoint")) Same(expect["checkpoint"]!, fakes.Stored ?? JsonValue.Null.Instance, "checkpoint");
        if (expect["saves"] is { } saves) Same(saves.AsInteger, (long)fakes.Saves, "saves");
        return true;
    }
}
