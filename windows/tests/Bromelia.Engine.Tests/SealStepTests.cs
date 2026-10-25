using System.Text;
using Bromelia.Domain;
using Bromelia.Foundation;
using Bromelia.Ports;
using Xunit;
using static Bromelia.Tests.Fixtures;
using CancellationToken = Bromelia.Foundation.CancellationToken;

namespace Bromelia.Engine.Tests;

/// <summary>shared/fixtures/engine/seal-steps.cases.json: SealStep alone, on a file tree in memory (a disk and a cache,
/// failures, a cancel on an operation), a fixed clock and the engine harnesses' fake renderer. The format is in
/// claudeDocs/tools/rebuild-phase3/gen-seal-cases.py.</summary>
public sealed class SealStepTests
{
    const string JobId = "00000000-0000-4000-8000-000000000001";
    static JsonValue.Object EmptyObject => new(new List<KeyValuePair<string, JsonValue>>());

    sealed class Fakes : IProgressReporter, IJobLogger, ILibraryPort, IClock
    {
        readonly JsonValue _given;
        public readonly MemoryFileSystem Fs;
        public readonly List<BroMessage> Log = new();
        public readonly List<JsonValue> Progress = new();

        public Fakes(JsonValue given, MemoryFileSystem fs)
        {
            _given = given;
            Fs = fs;
        }

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

        /// <summary>Seal's times are UTC (decision 59).</summary>
        public LocalTime LocalTime(Instant instant) => throw new NotSupportedException();
    }

    /// <summary>'&lt;code&gt;' then ' &lt;key&gt;=&lt;value&gt;' per param: strings as they are, other values as canonical JSON
    /// without its final newline.</summary>
    sealed class Renderer : IMessageRenderer
    {
        public string Render(BroMessage message) => MessageCode.Wire(message.Code) + string.Concat(message.Params.Members.Select(m =>
            " " + m.Key + "=" + (m.Value.AsString ?? Encoding.UTF8.GetString(JsonValue.EncodeCanonical(m.Value)).TrimEnd('\n'))));
    }

    static JobRequest RequestOf(JsonValue r) => new(
        EnumWire.Parse<JobKind>(r["kind"]?.AsString) ?? JobKind.VideoDisc, r["title"]?.AsString ?? "", r["automatic"]?.AsBool ?? false,
        r["driveId"]?.AsString, r["mediaGeneration"]?.AsInteger, null, Options: r["options"], Details: r["details"]);

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

    static Dictionary<string, string> Snapshot(MemoryFileSystem fs, string staging) => fs.Exists(staging)
        ? FileTree.Files(fs, staging).Where(f => !f.EndsWith('/')).ToDictionary(f => f, f => Encoding.UTF8.GetString(fs.Read(staging + "/" + f)))
        : new Dictionary<string, string>();

    [Fact]
    public void TheSealCasesPass() => RunCases("engine/seal-steps.cases.json", Case);

    static bool Case(string id, JsonValue given, JsonValue expect)
    {
        var step = new SealStep();
        var fs = new MemoryFileSystem();
        var fakes = new Fakes(given, fs);
        var staging = (given["libraries"]?["lib"]?["path"]?.AsString ?? "/lib") + "/.bromelia/staging/" + JobId;
        var jobDirectory = "/data/jobs/" + JobId;
        void PutStaging(string rel, JsonValue content)
        {
            if (rel.EndsWith('/')) fs.PutFolder(staging + "/" + rel.TrimEnd('/')); // an empty folder
            else if (content.AsString is { } text) fs.Put(staging + "/" + rel, Encoding.UTF8.GetBytes(text));
            else
            {
                fs.Put(staging + "/" + rel, Encoding.UTF8.GetBytes(content["disk"]!.AsString!));
                fs.Cached[staging + "/" + rel] = Encoding.UTF8.GetBytes(content["cache"]!.AsString!);
            }
        }
        foreach (var (rel, content) in given["staging"]?.AsObject ?? new List<KeyValuePair<string, JsonValue>>()) PutStaging(rel, content);
        foreach (var (name, text) in given["job"]?.AsObject ?? new List<KeyValuePair<string, JsonValue>>())
            fs.Put(jobDirectory + "/" + name, Encoding.UTF8.GetBytes(text.AsString!));
        foreach (var f in given["fails"]?.AsArray ?? new List<JsonValue>())
            fs.Fails.Add((f["op"]!.AsString!, f["path"]!.AsString!, new BroError(f["error"]!["code"]!.AsString!,
                (JsonValue.Object)(f["error"]!["params"] ?? EmptyObject))));
        var cancel = new CancellationSource();
        if (given["cancelAt"] is { } at)
            fs.OnOp = (op, path) =>
            {
                if (op == at["op"]!.AsString && path == at["path"]!.AsString) cancel.Cancel();
            };
        var outputs = (given["outputs"]?.AsArray ?? new List<JsonValue>())
            .Select(o => new StepOutput(EnumWire.Parse<StepKind>(o["kind"]!.AsString)!.Value, o["data"]!, null, (int)(o["readErrors"]?.AsInteger ?? 0)))
            .ToList();
        var job = new JobSnapshot(new Id(JobId), RequestOf(given["request"]!), null, outputs, Instant.Parse(given["startedAt"]!.AsString!));
        var context = new StepContext(job, cancel.Token, fakes, fakes, null!, new ToolSet(null!, null!), fs, fakes, null!, fakes, null!, null!, null!,
            null!, null!, null!, jobDirectory, new Renderer(), given["appVersion"]!.AsString!);

        if (expect["resources"] is { } resources)
            Same(resources["libraryWrites"], JsonValue.Of(step.Resources(job).LibraryWrites.Select(d => JsonValue.Of(d))), "resources.libraryWrites");
        if (expect["resumes"] is { } resumes) Same(resumes["resumability"]!.AsString, EnumWire.Name(step.Resumability), "resumability");

        int runs = (int)(given["runs"]?.AsInteger ?? 1);
        Dictionary<string, string>? first = null;
        StepReport? report = null;
        BroError? thrown = null;
        for (int run = 1; run <= runs; run++)
        {
            if (run == runs)
            {
                if (run > 1)
                    foreach (var (rel, content) in given["between"]?.AsObject ?? new List<KeyValuePair<string, JsonValue>>()) PutStaging(rel, content);
                fs.Ops.Clear();
                fs.Reads.Clear();
                fakes.Log.Clear();
                fakes.Progress.Clear();
                fs.Recording = true;
            }
            (report, thrown) = Run(step, context);
            fs.Recording = false;
            if (run == 1 && runs > 1)
            {
                if ((thrown ?? report?.Error) is { } e) throw new Xunit.Sdk.XunitException($"the first run failed: {e.Code} {e.Params}");
                first = Snapshot(fs, staging);
            }
        }

        var error = thrown ?? report?.Error;
        if (expect["error"] is { } expectedError)
        {
            Same(expectedError["code"]!.AsString, error?.Code, "error");
            Same(expectedError["params"] ?? EmptyObject, (JsonValue?)error?.Params ?? EmptyObject, "error params");
        }
        else if (error != null) throw new Xunit.Sdk.XunitException($"unexpected error {error.Code} {error.Params}");
        Same(expect["skipped"]?.AsBool ?? false, report?.Skipped ?? false, "skipped");
        if (expect["data"] is { } data) Subset(data, report?.Data, "data");
        if (expect["log"] is { } log)
        {
            var got = fakes.Log;
            Same(log.AsArray!.Count, got.Count, "log length (" + string.Join(", ", got.Select(m => MessageCode.Wire(m.Code))) + ")");
            for (int i = 0; i < got.Count; i++)
            {
                var e = log.AsArray[i];
                Same(e["code"]!.AsString, MessageCode.Wire(got[i].Code), $"log[{i}]");
                Same(e["params"] ?? EmptyObject, (JsonValue)got[i].Params, $"log[{i}] params");
                if (e["severity"]?.AsString is { } sev) Same(sev, EnumWire.Name(got[i].Severity), $"log[{i}] severity");
            }
        }
        if (expect["progress"] is { } progress)
        {
            Same(progress.AsArray!.Count, fakes.Progress.Count, "progress length");
            Subset(progress, JsonValue.Of(fakes.Progress), "progress");
        }
        if (expect["fs"] is { } ops) Same(string.Join("\n", ops.AsArray!.Select(o => o.AsString)), string.Join("\n", fs.Ops), "fs");
        foreach (var (rel, n) in expect["reads"]?.AsObject ?? new List<KeyValuePair<string, JsonValue>>())
            Same(n.AsInteger, (long)fs.Reads.GetValueOrDefault(staging + "/" + rel), "reads of " + rel);
        Same(0L, (long)fs.OpenStreams, "streams left open"); // every case: a handle left open blocks Commit's move on Windows
        var after = Snapshot(fs, staging);
        if (expect["tree"] is { } tree)
            Same(tree, JsonValue.Of((fs.Exists(staging) ? FileTree.Files(fs, staging) : new List<string>()).Select(f => JsonValue.Of(f))), "tree");
        foreach (var (rel, text) in expect["contents"]?.AsObject ?? new List<KeyValuePair<string, JsonValue>>())
            Same(text.AsString, after.GetValueOrDefault(rel), "contents of " + rel);
        if (expect["record"]?.AsString is { } record)
        {
            var name = report?.Data["record"]?.AsString ?? "";
            Same(record, after.GetValueOrDefault(name), "record " + name);
        }
        if (expect["sameAsFirst"]?.AsBool == true)
        {
            Same(string.Join("\n", first!.Keys.OrderBy(k => k, StringComparer.Ordinal)), string.Join("\n", after.Keys.OrderBy(k => k, StringComparer.Ordinal)),
                "the files after the rerun");
            foreach (var (rel, text) in first) Same(text, after[rel], "the rerun's " + rel);
        }
        return true;
    }
}
