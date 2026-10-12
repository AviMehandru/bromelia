using System.Text;
using Bromelia.Domain;
using Bromelia.Foundation;
using Bromelia.Ports;
using Xunit;
using static Bromelia.Tests.Fixtures;
using FileInfo = Bromelia.Ports.FileInfo;

namespace Bromelia.Engine.Tests;

/// <summary>shared/fixtures/engine/config-service.cases.json, on a real executor with fake ports.</summary>
public sealed class ConfigServiceTests
{
    const string ConfigPath = "/data/config.json";
    static readonly TimeSpan Patience = TimeSpan.FromSeconds(10);

    static BroFailure Failure(JsonValue j) =>
        new(new BroError(j["code"]!.AsString!, j["params"] as JsonValue.Object ?? new JsonValue.Object(new List<KeyValuePair<string, JsonValue>>())));

    static JsonValue ErrorJson(BroError e) => JsonValue.Of(("error", JsonValue.Of(("code", JsonValue.Of(e.Code)), ("params", e.Params))));

    /// <summary>A document as ConfigCodec.decode leaves it, so a case can write it short.</summary>
    static JsonValue Norm(JsonValue doc) => ConfigCodec.Decode(JsonValue.EncodeCanonical(doc)).Document;

    sealed class Files : IFileSystem
    {
        readonly object _lock = new();
        public byte[]? Bytes;
        public int? Mode;
        public JsonValue? WriteFails;
        public ManualResetEventSlim? Hold;
        public readonly ManualResetEventSlim Held = new();

        public bool Exists(string path)
        {
            lock (_lock) return path == ConfigPath && Bytes != null;
        }

        public byte[] Read(string path)
        {
            lock (_lock) return Bytes ?? throw new BroFailure(new BroError("fs.notFound", ("path", JsonValue.Of(path))));
        }

        public void WriteAtomically(string path, byte[] bytes, int mode)
        {
            Assert.Equal(ConfigPath, path);
            WaitIfHeld();
            if (WriteFails != null) throw Failure(WriteFails);
            lock (_lock)
            {
                Bytes = bytes.ToArray();
                Mode = mode;
            }
        }

        /// <summary>With holdWrites, every write (kv or file) waits here for releaseWrites.</summary>
        public void WaitIfHeld()
        {
            if (Hold == null) return;
            Held.Set();
            if (!Hold.Wait(Patience)) throw new TimeoutException("the write was never released");
        }

        public FileInfo Stat(string path) => throw new NotSupportedException();
        public IReadOnlyList<DirectoryEntry> List(string directory) => throw new NotSupportedException();
        public byte[] ReadRange(string path, long offset, int length) => throw new NotSupportedException();
        public void CreateDirectory(string path, bool parentsMustExist) => throw new NotSupportedException();
        public void Rename(string from, string to) => throw new NotSupportedException();
        public IReadOnlyList<MovedItem> MoveMerging(string from, string to, MovePolicy policy, Action<MovedItem>? onMoved = null) => throw new NotSupportedException();
        public void Remove(string path) => throw new NotSupportedException();
        public void MoveToTrash(string path, string trash) => throw new NotSupportedException();
        public void SyncFile(string path) => throw new NotSupportedException();
        public void SyncDirectory(string path) => throw new NotSupportedException();
        public IByteStream OpenForReading(string path, bool bypassCache) => throw new NotSupportedException();
        public VolumeInfo Volume(string path) => throw new NotSupportedException();
    }

    sealed class Keystore : IKeystore
    {
        readonly object _lock = new();
        public readonly SortedDictionary<string, string> Values = new(StringComparer.Ordinal);
        public readonly Dictionary<string, JsonValue> Fails = new();

        void Check(string name)
        {
            if (Fails.TryGetValue(name, out var f)) throw Failure(f);
        }

        public string? Get(string name)
        {
            Check(name);
            lock (_lock) return Values.GetValueOrDefault(name);
        }

        public void Set(string name, string value)
        {
            Check(name);
            lock (_lock) Values[name] = value;
        }

        public void Remove(string name)
        {
            Check(name);
            lock (_lock) Values.Remove(name);
        }
    }

    sealed class Kv : IKeyValueRepository
    {
        readonly object _lock = new();
        public readonly SortedDictionary<string, JsonValue> Values = new(StringComparer.Ordinal);
        public Files? Gate;

        public Task<JsonValue?> Get(string key)
        {
            lock (_lock) return Task.FromResult(Values.GetValueOrDefault(key));
        }

        public Task Set(string key, JsonValue value)
        {
            Gate?.WaitIfHeld();
            lock (_lock) Values[key] = value;
            return Task.CompletedTask;
        }
    }

    [Fact]
    public void TheSharedCasesPass()
    {
        RunCases("engine/config-service.cases.json", (id, given, expect) =>
        {
            using var w = new EngineClockTests.World();
            var bus = new EventBus(w.Clock);
            var problems = new ProblemService(bus, w.Clock);
            var files = new Files();
            if (given["file"] is { } file)
                files.Bytes = file["text"]?.AsString is { } text ? Encoding.UTF8.GetBytes(text) : JsonValue.EncodeCanonical(file);
            if (given["writeFails"] is { } wf) files.WriteFails = wf;
            if (given["holdWrites"]?.AsBool == true) files.Hold = new ManualResetEventSlim();
            var keystore = new Keystore();
            foreach (var (k, v) in given["keystore"]?.AsObject ?? new List<KeyValuePair<string, JsonValue>>()) keystore.Values[k] = v.AsString!;
            foreach (var (k, v) in given["keystoreFails"]?.AsObject ?? new List<KeyValuePair<string, JsonValue>>()) keystore.Fails[k] = v;
            var kv = new Kv { Gate = files };
            foreach (var (k, v) in given["kv"]?.AsObject ?? new List<KeyValuePair<string, JsonValue>>()) kv.Values[k] = v;
            var environment = (given["environment"]?.AsObject ?? new List<KeyValuePair<string, JsonValue>>()).ToDictionary(m => m.Key, m => m.Value.AsString!);
            var service = new ConfigService(w.Executor, bus, problems, files, keystore, kv, ConfigPath, environment);
            var events = new List<EngineEvent>();
            w.OnExecutor(() => bus.Subscribe(null, events.Add));

            var ops = given["ops"]!.AsArray!;
            var results = new JsonValue?[ops.Count];
            var done = new List<ManualResetEventSlim>();

            void Wait(ManualResetEventSlim d)
            {
                if (!d.Wait(Patience)) throw new TimeoutException("an op didn't finish");
            }

            // Starts an asynchronous op on the executor; its result lands in results[i].
            ManualResetEventSlim Start(int i, Func<Task<JsonValue>> op)
            {
                var d = new ManualResetEventSlim();
                done.Add(d);
                w.Executor.Post(async () =>
                {
                    try { results[i] = await op(); }
                    catch (BroFailure f) { results[i] = ErrorJson(f.Error); }
                    d.Set();
                });
                return d;
            }

            JsonValue Sync(Func<JsonValue> op)
            {
                JsonValue? r = null;
                w.OnExecutor(() =>
                {
                    try { r = op(); }
                    catch (BroFailure f) { r = ErrorJson(f.Error); }
                });
                return r!;
            }

            for (int i = 0; i < ops.Count; i++)
            {
                var op = ops[i];
                var (name, arg) = op is JsonValue.String s ? new KeyValuePair<string, JsonValue>(s.Value, JsonValue.Null.Instance) : op.AsObject!.Single();
                switch (name)
                {
                    case "load":
                        Wait(Start(i, async () => { await service.Load(); return JsonValue.Of("ok"); }));
                        break;
                    case "update":
                    {
                        var d = Start(i, async () => ConfigEnvelope.Json(await service.Update(arg["doc"]!, arg["version"]!.AsInteger!.Value)));
                        if (arg["wait"]?.AsBool != false) Wait(d);
                        else if (!files.Held.Wait(Patience)) throw new TimeoutException("the update didn't reach its write");
                        break;
                    }
                    case "envelope":
                        Wait(Start(i, async () => ConfigEnvelope.Json(await service.Envelope())));
                        break;
                    case "setSecret":
                        Wait(Start(i, async () => { await service.SetSecret(arg["name"]!.AsString!, arg["value"]!.AsString!); return JsonValue.Of("ok"); }));
                        break;
                    case "validate":
                        results[i] = Sync(() => JsonValue.Of(service.Validate(arg).Select(x => x.ToJson())));
                        break;
                    case "current":
                        results[i] = Sync(() => service.Current().Document);
                        break;
                    case "version":
                        results[i] = Sync(() => JsonValue.Of(service.Version()));
                        break;
                    case "export":
                        results[i] = Sync(() => JsonValue.Parse(service.Export())!);
                        break;
                    case "secret":
                        // Not on the executor: the keystore may block.
                        try { results[i] = JsonValue.Of(service.Secret(arg.AsString!)); }
                        catch (BroFailure f) { results[i] = ErrorJson(f.Error); }
                        break;
                    case "releaseWrites":
                        files.Hold!.Set();
                        foreach (var d in done) Wait(d);
                        break;
                    default:
                        throw new InvalidOperationException("unknown op " + name);
                }
            }
            foreach (var d in done) Wait(d);
            w.Settle();

            var expectedResults = expect["results"]!.AsArray!;
            var r = 0;
            for (int i = 0; i < ops.Count; i++)
            {
                if (ops[i].AsString == "releaseWrites") continue;
                var want = expectedResults[r++];
                var name = ops[i].AsString ?? ops[i].AsObject!.Single().Key;
                if (want["error"] == null)
                {
                    if (name is "update" or "envelope") want = Replace(want, "doc", Norm(want["doc"]!));
                    else if (name == "current") want = Norm(want);
                    else if (name == "export") want = Replace(want, "config", Norm(want["config"]!));
                }
                Same(Sorted(want), Sorted(results[i] ?? JsonValue.Null.Instance), $"result of op {i} ({name})");
            }
            Same(expectedResults.Count, r, "number of results");

            var wantFile = expect["file"]!;
            if (wantFile.IsNull) Same(null, files.Bytes, "file");
            else if (wantFile["text"]?.AsString is { } text) Same(text, Encoding.UTF8.GetString(files.Bytes!), "file");
            else
            {
                Same(Sorted(Norm(wantFile)), Sorted(Norm(JsonValue.Parse(files.Bytes!)!)), "file");
                if (files.Mode != null)
                {
                    Same(0x180, files.Mode.Value, "file mode");
                    Same(Encoding.UTF8.GetString(ConfigCodec.Encode(ConfigCodec.Decode(JsonValue.EncodeCanonical(wantFile)))),
                        Encoding.UTF8.GetString(files.Bytes!), "file bytes");
                }
            }
            var wantKv = expect["kv"]!;
            if (wantKv["config.lastGood"] is { } lastGood) wantKv = Replace(wantKv, "config.lastGood", Norm(lastGood));
            var gotKv = new JsonValue.Object(kv.Values.ToList());
            if (gotKv["config.lastGood"] is { } stored) gotKv = (JsonValue.Object)Replace(gotKv, "config.lastGood", Norm(stored));
            Same(Sorted(wantKv), Sorted(gotKv), "kv");
            Same(Sorted(expect["keystore"]!), Sorted(JsonValue.Of(keystore.Values.Select(p => (p.Key, JsonValue.Of(p.Value))).ToArray())), "keystore");
            JsonValue? got = null;
            w.OnExecutor(() => got = JsonValue.Of(
                ("problems", JsonValue.Of(problems.Problems().Select(ProblemDto.Json))),
                ("events", JsonValue.Of(events.Select(EngineEvent.Json)))));
            Same(Sorted(expect["events"]!), Sorted(got!["events"]!), "events");
            Same(Sorted(expect["problems"]!), Sorted(got!["problems"]!), "problems");
            return true;
        });
    }

    static JsonValue Replace(JsonValue obj, string key, JsonValue value) =>
        new JsonValue.Object(obj.AsObject!.Select(m => m.Key == key ? new KeyValuePair<string, JsonValue>(key, value) : m).ToList());
}
