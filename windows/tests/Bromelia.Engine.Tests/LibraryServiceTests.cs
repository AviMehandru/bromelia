using System.Text;
using Bromelia.Domain;
using Bromelia.Foundation;
using Bromelia.Ports;
using Xunit;
using static Bromelia.Tests.Fixtures;
using FileInfo = Bromelia.Ports.FileInfo;

namespace Bromelia.Engine.Tests;

/// <summary>shared/fixtures/engine/library-service.cases.json, on a real executor, scheduler and problem service, with a
/// file system and database in memory.</summary>
public sealed class LibraryServiceTests
{
    static readonly TimeSpan Patience = TimeSpan.FromSeconds(10);
    const long Free = 5_000_000_000;

    static JsonValue ErrorJson(BroError e)
    {
        var m = new List<KeyValuePair<string, JsonValue>> { new("code", JsonValue.Of(e.Code)) };
        if (e.Params.Members.Count > 0) m.Add(new("params", e.Params));
        return JsonValue.Of(("error", new JsonValue.Object(m)));
    }

    static BroFailure Failure(JsonValue j) =>
        new(new BroError(j["code"]!.AsString!, j["params"] as JsonValue.Object ?? new JsonValue.Object(new List<KeyValuePair<string, JsonValue>>())));

    /// <summary>Thrown at the crash point: the commit stops there, as a killed engine would.</summary>
    sealed class Crash : Exception
    {
        public readonly string Point;
        public Crash(string point) : base("crashed at " + point) { Point = point; }
    }

    /// <summary>Files and folders in memory, with the platform adapters' rules; calls can be made to fail or wait.</summary>
    sealed class Tree : IFileSystem
    {
        readonly object _lock = new();
        public readonly SortedDictionary<string, byte[]> Files = new(StringComparer.Ordinal);
        public readonly SortedSet<string> Dirs = new(StringComparer.Ordinal) { "/" };
        public readonly SortedDictionary<string, int> Modes = new(StringComparer.Ordinal);
        public readonly SortedSet<string> Synced = new(StringComparer.Ordinal);
        public readonly List<(string Op, string Path, JsonValue Error, int Times)> Fails = new();
        public readonly List<(string Op, string Path)> Holds = new();
        public readonly ManualResetEventSlim Gate = new();
        public int Held;

        static string Parent(string p) => p.LastIndexOf('/') == 0 ? "/" : p[..p.LastIndexOf('/')];

        static BroFailure Error(MessageCode code, params (string, JsonValue)[] p) => new(new BroMessage(code, Severity.Error, p).ToError());

        void Before(string op, string path)
        {
            lock (_lock)
            {
                for (int i = 0; i < Fails.Count; i++)
                {
                    var f = Fails[i];
                    if (f.Op != op || f.Path != path || f.Times == 0) continue;
                    if (f.Times > 0) Fails[i] = f with { Times = f.Times - 1 };
                    throw Failure(f.Error);
                }
            }
            if (Gate.IsSet || !Holds.Contains((op, path))) return;
            Interlocked.Increment(ref Held);
            var released = Gate.Wait(Patience);
            Interlocked.Decrement(ref Held);
            if (!released) throw new TimeoutException("a held call was never released");
        }

        public void MakeDirs(string dir)
        {
            lock (_lock)
                for (var d = dir; d != "/" && Dirs.Add(d); d = Parent(d)) { }
        }

        public void Put(string path, string text)
        {
            MakeDirs(Parent(path));
            lock (_lock) Files[path] = Encoding.UTF8.GetBytes(text);
        }

        public void MoveAway(string path) => Move(path, path + ".away");

        void Move(string from, string to)
        {
            lock (_lock)
            {
                foreach (var f in Files.Keys.Where(k => k == from || k.StartsWith(from + "/", StringComparison.Ordinal)).ToList())
                {
                    Files[to + f[from.Length..]] = Files[f];
                    Files.Remove(f);
                    if (Modes.Remove(f, out var mode)) Modes[to + f[from.Length..]] = mode;
                }
                foreach (var d in Dirs.Where(k => k == from || k.StartsWith(from + "/", StringComparison.Ordinal)).ToList())
                {
                    Dirs.Remove(d);
                    Dirs.Add(to + d[from.Length..]);
                }
            }
        }

        public bool Exists(string path)
        {
            Before("exists", path);
            lock (_lock) return Files.ContainsKey(path) || Dirs.Contains(path);
        }

        public FileInfo Stat(string path)
        {
            Before("stat", path);
            lock (_lock)
            {
                if (Files.TryGetValue(path, out var b)) return new FileInfo(b.Length, false, new Instant(0));
                if (Dirs.Contains(path)) return new FileInfo(0, true, new Instant(0));
            }
            throw Error(MessageCode.FsNotFound, ("path", JsonValue.Of(path)));
        }

        public IReadOnlyList<DirectoryEntry> List(string directory)
        {
            Before("list", directory);
            lock (_lock)
            {
                if (!Dirs.Contains(directory)) throw Error(MessageCode.FsNotFound, ("path", JsonValue.Of(directory)));
                bool Child(string p) => p != directory && Parent(p) == directory;
                return Dirs.Where(Child).Select(d => new DirectoryEntry(d[(d.LastIndexOf('/') + 1)..], true))
                    .Concat(Files.Keys.Where(Child).Select(f => new DirectoryEntry(f[(f.LastIndexOf('/') + 1)..], false))).ToList();
            }
        }

        public byte[] Read(string path)
        {
            Before("read", path);
            lock (_lock)
                return Files.TryGetValue(path, out var b) ? b.ToArray() : throw Error(MessageCode.FsNotFound, ("path", JsonValue.Of(path)));
        }

        public void CreateDirectory(string path, bool parentsMustExist)
        {
            Before("createDirectory", path);
            lock (_lock)
            {
                if (Files.ContainsKey(path)) throw Error(MessageCode.FsAlreadyExists, ("path", JsonValue.Of(path)));
                if (Dirs.Contains(path)) return;
                if (parentsMustExist && !Dirs.Contains(Parent(path))) throw Error(MessageCode.FsParentMissing, ("path", JsonValue.Of(Parent(path))));
            }
            MakeDirs(path);
        }

        public void WriteAtomically(string path, byte[] bytes, int mode)
        {
            Before("writeAtomically", path);
            lock (_lock)
            {
                if (!Dirs.Contains(Parent(path))) throw Error(MessageCode.FsParentMissing, ("path", JsonValue.Of(Parent(path))));
                Files[path] = bytes.ToArray();
                Modes[path] = mode;
            }
        }

        public void Rename(string from, string to)
        {
            Before("rename", from);
            lock (_lock)
            {
                if (!Files.ContainsKey(from) && !Dirs.Contains(from)) throw Error(MessageCode.FsNotFound, ("path", JsonValue.Of(from)));
                if (Files.ContainsKey(to) || Dirs.Contains(to)) throw Error(MessageCode.FsAlreadyExists, ("path", JsonValue.Of(to)));
                if (!Dirs.Contains(Parent(to))) throw Error(MessageCode.FsParentMissing, ("path", JsonValue.Of(Parent(to))));
            }
            Move(from, to);
        }

        public IReadOnlyList<MovedItem> MoveMerging(string from, string to, MovePolicy policy, Action<MovedItem>? onMoved = null)
        {
            Before("moveMerging", from);
            if (!Exists(from)) throw Error(MessageCode.FsNotFound, ("path", JsonValue.Of(from)));
            if (!Stat(from).IsDirectory) throw Error(MessageCode.FsFailed, ("operation", JsonValue.Of("move")), ("path", JsonValue.Of(from)),
                ("reason", JsonValue.Of("it isn't a folder")));
            if (!Exists(to)) CreateDirectory(to, parentsMustExist: true);
            else if (!Stat(to).IsDirectory) throw Error(MessageCode.FsAlreadyExists, ("path", JsonValue.Of(to)));
            var moved = new List<MovedItem>();
            Merge(from, to, m =>
            {
                moved.Add(m);
                onMoved?.Invoke(m);
            });
            return moved.OrderBy(m => m.From, StringComparer.Ordinal).ToList();
        }

        void Merge(string source, string target, Action<MovedItem> moved)
        {
            foreach (var e in List(source).OrderBy(e => e.Name, StringComparer.Ordinal))
            {
                var src = source + "/" + e.Name;
                var existing = List(target);
                var same = existing.FirstOrDefault(x => x.Name.ToLowerInvariant() == e.Name.ToLowerInvariant());
                if (e.IsDirectory && same is { IsDirectory: true })
                {
                    Merge(src, target + "/" + same.Name, moved);
                    Remove(src);
                    continue;
                }
                var dst = target + "/" + ConflictNamer.Next(e.Name, existing.Select(x => x.Name).ToList(), e.IsDirectory);
                Rename(src, dst);
                if (!e.IsDirectory) moved(new MovedItem(src, dst));
                else
                    lock (_lock)
                        foreach (var f in Files.Keys.Where(k => k.StartsWith(dst + "/", StringComparison.Ordinal)).ToList())
                            moved(new MovedItem(src + f[dst.Length..], f));
            }
        }

        public void Remove(string path)
        {
            Before("remove", path);
            lock (_lock)
            {
                if (Files.Remove(path)) return;
                if (!Dirs.Contains(path)) throw Error(MessageCode.FsNotFound, ("path", JsonValue.Of(path)));
                if (Files.Keys.Concat(Dirs).Any(k => k.StartsWith(path + "/", StringComparison.Ordinal)))
                    throw Error(MessageCode.FsFailed, ("operation", JsonValue.Of("remove")), ("path", JsonValue.Of(path)),
                        ("reason", JsonValue.Of("the folder isn't empty")));
                Dirs.Remove(path);
            }
        }

        public void SyncDirectory(string path)
        {
            Before("syncDirectory", path);
            lock (_lock)
            {
                if (!Dirs.Contains(path)) throw Error(MessageCode.FsNotFound, ("path", JsonValue.Of(path)));
                Synced.Add(path);
            }
        }

        sealed class Stream : IByteStream
        {
            readonly byte[] _bytes;
            int _at;
            public Stream(byte[] bytes) { _bytes = bytes; }

            public byte[] Read(int maxBytes)
            {
                var n = Math.Min(Math.Min(maxBytes, 4), _bytes.Length - _at);
                var chunk = _bytes[_at..(_at + n)];
                _at += n;
                return chunk;
            }

            public void Close() { }
        }

        public IByteStream OpenForReading(string path, bool bypassCache) => new Stream(Read(path));

        public VolumeInfo Volume(string path)
        {
            Before("volume", path);
            return new VolumeInfo("volume", Free, "apfs", true);
        }

        public byte[] ReadRange(string path, long offset, int length) => throw new NotSupportedException();
        public void MoveToTrash(string path, string trash) => throw new NotSupportedException();
        public void SyncFile(string path) => throw new NotSupportedException();
    }

    /// <summary>The libraries table and the unit tables in memory; a unit's library must be stored (the foreign key).</summary>
    sealed class Db : ILibraryRepository, IUnitRepository
    {
        readonly object _lock = new();
        public readonly SortedDictionary<string, LibraryRecord> Libraries = new(StringComparer.Ordinal);
        public readonly SortedDictionary<string, UnitRecord> Units = new(StringComparer.Ordinal);
        public readonly Dictionary<Id, CommitIntent> Intents = new();
        public readonly SortedDictionary<string, IReadOnlyList<UnitFile>> Files = new(StringComparer.Ordinal);

        public Task<IReadOnlyList<LibraryRecord>> All()
        {
            lock (_lock) return Task.FromResult<IReadOnlyList<LibraryRecord>>(Libraries.Values.ToList());
        }

        public Task<LibraryRecord?> Get(string libraryId)
        {
            lock (_lock) return Task.FromResult(Libraries.GetValueOrDefault(libraryId));
        }

        public Task Upsert(LibraryRecord library)
        {
            lock (_lock) Libraries[library.Id] = library;
            return Task.CompletedTask;
        }

        public Task SetSeen(string libraryId, bool online, Instant at)
        {
            lock (_lock)
                if (Libraries.TryGetValue(libraryId, out var l))
                    Libraries[libraryId] = l with { Online = online, LastSeenAt = online ? at : l.LastSeenAt };
            return Task.CompletedTask;
        }

        public Task BeginCommit(UnitRecord unit, CommitIntent intent)
        {
            lock (_lock)
            {
                if (!Libraries.ContainsKey(unit.LibraryId)) throw new BroFailure(new BroError("store.failed", ("reason", JsonValue.Of("FOREIGN KEY"))));
                Units[unit.Id.Value] = unit;
                Intents[unit.Id] = intent;
            }
            return Task.CompletedTask;
        }

        public Task MarkMoved(Id unitId, int seq)
        {
            lock (_lock)
            {
                var i = Intents[unitId];
                Intents[unitId] = i with { Items = i.Items.Select(x => x.Seq == seq ? x with { Moved = true } : x).ToList() };
            }
            return Task.CompletedTask;
        }

        public Task FinishCommit(CommitIntent intent, UnitRecord record, IReadOnlyList<UnitFile> files)
        {
            lock (_lock)
            {
                Units[record.Id.Value] = record;
                Files[record.Id.Value] = files.ToList();
                Intents.Remove(intent.UnitId);
            }
            return Task.CompletedTask;
        }

        public Task<IReadOnlyList<CommitIntent>> OpenIntents()
        {
            lock (_lock)
                return Task.FromResult<IReadOnlyList<CommitIntent>>(Intents.Values.OrderBy(i => i.CreatedAt.UnixMilliseconds)
                    .ThenBy(i => i.UnitId.Value, StringComparer.Ordinal).ToList());
        }

        Task<UnitRecord?> IUnitRepository.Get(Id unitId)
        {
            lock (_lock) return Task.FromResult(Units.GetValueOrDefault(unitId.Value));
        }

        public Task<IReadOnlyList<UnitRecord>> Query(UnitQuery query) => throw new NotSupportedException();
        public Task<IReadOnlyList<UnitRecord>> LeastRecentlyVerified(int limit) => throw new NotSupportedException();
        public Task MarkMissing(Id unitId) => throw new NotSupportedException();
    }

    sealed class ConfigBox : IConfigSource
    {
        public Config Value = Of(JsonValue.Of());
        public Config Current() => Value;

        public static Config Of(JsonValue libraries) =>
            ConfigCodec.Decode(JsonValue.EncodeCanonical(JsonValue.Of(("version", JsonValue.Of(3L)), ("libraries", libraries is JsonValue.Array ? libraries : JsonValue.Of(Array.Empty<JsonValue>())))));
    }

    static JsonValue Row(LibraryRecord l)
    {
        var m = new List<KeyValuePair<string, JsonValue>>
        {
            new("id", JsonValue.Of(l.Id)), new("name", JsonValue.Of(l.Name)), new("path", JsonValue.Of(l.Path)), new("markerId", JsonValue.Of(l.MarkerId)),
            new("attachedAt", JsonValue.Of(Instant.Format(l.AttachedAt))), new("online", JsonValue.Of(l.Online)),
            new("lastSeenAt", l.LastSeenAt is { } s ? JsonValue.Of(Instant.Format(s)) : JsonValue.Null.Instance),
        };
        return new JsonValue.Object(m);
    }

    static JsonValue Intents(IEnumerable<CommitIntent> intents) => JsonValue.Of(intents.Select(i => JsonValue.Of(new[]
    {
        JsonValue.Of(i.UnitId.Value),
        JsonValue.Of(i.Items.OrderBy(x => x.Seq).Select(x => JsonValue.Of($"{x.Seq} {x.FromPath} {x.ToPath}{(x.IsDir ? " dir" : "")}{(x.Moved ? " moved" : "")}"))),
    })));

    static IReadOnlyList<CommitIntent> OpenNow(Db db) => db.OpenIntents().Result;

    static Lease Granted(Task<Lease> lease) => lease.Result;

    static JsonValue FileRows(IEnumerable<UnitFile> files) => JsonValue.Of(files.Select(f =>
        JsonValue.Of(new[] { JsonValue.Of(f.Path), JsonValue.Of(f.Size), JsonValue.Of(f.Sha256), JsonValue.Of(f.Role) })));

    static JsonValue ResultJson(CommitResult r) => JsonValue.Of(("unitId", JsonValue.Of(r.UnitId.Value)), ("destination", JsonValue.Of(r.Destination)),
        ("files", JsonValue.Of(r.Files.Select(f => f.Path).OrderBy(p => p, StringComparer.Ordinal).Select(JsonValue.Of))), ("merged", JsonValue.Of(r.Merged)));

    [Fact]
    public void TheSharedCasesPass()
    {
        RunCases("engine/library-service.cases.json", (id, given, expect) =>
        {
            using var w = new EngineClockTests.World();
            var tree = new Tree();
            foreach (var d in given["dirs"]?.AsArray ?? new List<JsonValue>()) tree.MakeDirs(d.AsString!);
            foreach (var (p, t) in given["files"]?.AsObject ?? new List<KeyValuePair<string, JsonValue>>()) tree.Put(p, t.AsString!);
            foreach (var f in given["fail"]?.AsArray ?? new List<JsonValue>())
                tree.Fails.Add((f["op"]!.AsString!, f["path"]!.AsString!, f["error"]!, (int)(f["times"]?.AsInteger ?? -1)));
            foreach (var h in given["hold"]?.AsArray ?? new List<JsonValue>()) tree.Holds.Add((h["op"]!.AsString!, h["path"]!.AsString!));
            var db = new Db();
            foreach (var l in given["libraries"]?.AsArray ?? new List<JsonValue>())
                db.Libraries[l["id"]!.AsString!] = new LibraryRecord(l["id"]!.AsString!, l["name"]!.AsString!, l["path"]!.AsString!, l["markerId"]!.AsString!,
                    Instant.Parse(l["attachedAt"]!.AsString!)!.Value, l["online"]!.AsBool!.Value, l["lastSeenAt"]?.AsString is { } s ? Instant.Parse(s) : null);
            var config = new ConfigBox { Value = ConfigBox.Of(given["config"] ?? JsonValue.Of(Array.Empty<JsonValue>())) };
            var crashAt = given["crashAt"]?.AsString;
            var crashed = false;
            void CrashPoint(string point)
            {
                if (point != crashAt || crashed) return;
                crashed = true;
                throw new Crash(point);
            }

            var events = new List<EngineEvent>();
            EventBus bus = null!;
            ProblemService problems = null!;
            Scheduler scheduler = null!;
            LibraryService service = null!;
            void Build()
            {
                bus = new EventBus(w.Clock);
                problems = new ProblemService(bus, w.Clock);
                scheduler = new Scheduler(w.Executor, bus, new SchedulerLimits());
                service = new LibraryService(w.Executor, bus, problems, scheduler, config, w.EngineClock, tree, db, db, CrashPoint);
                w.OnExecutor(() => bus.Subscribe(new[] { EngineEventType.LibraryChanged, EngineEventType.ProblemRaised, EngineEventType.ProblemCleared,
                    EngineEventType.ConfigChanged }, events.Add));
            }
            Build();

            bool Settled()
            {
                var ok = false;
                var checkedIt = new ManualResetEventSlim();
                w.Executor.Post(() =>
                {
                    ok = w.Executor.Activity == Volatile.Read(ref tree.Held);
                    checkedIt.Set();
                });
                if (!checkedIt.Wait(Patience)) throw new TimeoutException("the executor didn't run the check");
                return ok;
            }
            void Settle()
            {
                var until = DateTime.UtcNow + Patience;
                while (!Settled())
                {
                    if (DateTime.UtcNow > until) throw new TimeoutException("the service didn't settle");
                    Thread.Sleep(1);
                }
            }
            w.Clock.Settle = Settle;

            var ops = given["ops"]!.AsArray!;
            var results = new JsonValue?[ops.Count];
            var done = new List<ManualResetEventSlim>();
            void Wait(ManualResetEventSlim d)
            {
                if (!d.Wait(Patience)) throw new TimeoutException("an op didn't finish");
            }
            JsonValue Catching(Func<JsonValue> op)
            {
                try { return op(); }
                catch (BroFailure f) { return ErrorJson(f.Error); }
                catch (Crash c) { return JsonValue.Of(("crashed", JsonValue.Of(c.Point))); }
            }
            async Task<JsonValue> CatchingAsync(Func<Task<JsonValue>> op)
            {
                try { return await op(); }
                catch (BroFailure f) { return ErrorJson(f.Error); }
                catch (Crash c) { return JsonValue.Of(("crashed", JsonValue.Of(c.Point))); }
            }
            // An op that runs on the executor and may wait; its result lands in results[i].
            void OnExecutor(int i, JsonValue arg, Func<Task<JsonValue>> op)
            {
                var d = new ManualResetEventSlim();
                done.Add(d);
                w.Executor.Post(async () =>
                {
                    results[i] = await CatchingAsync(op);
                    d.Set();
                });
                if (arg["wait"]?.AsBool != false) Wait(d);
                Settle();
            }
            // An op that runs off the executor (the port, as a step calls it).
            void OffExecutor(int i, JsonValue arg, Func<Task<JsonValue>> op)
            {
                var d = new ManualResetEventSlim();
                done.Add(d);
                var task = Task.Run(async () =>
                {
                    results[i] = await CatchingAsync(op);
                    d.Set();
                });
                if (arg["wait"]?.AsBool != false) Wait(d);
                else
                {
                    var until = DateTime.UtcNow + Patience;
                    while (Volatile.Read(ref tree.Held) == 0 && !d.IsSet)
                    {
                        if (DateTime.UtcNow > until) throw new TimeoutException("the op never reached its held call");
                        Thread.Sleep(1);
                    }
                }
                Settle();
            }
            UnitRecord Unit(JsonValue u, int files) => new(new Id(u["id"]!.AsString!), "lib", null, new Id("00000000-0000-4000-8000-000000000001"),
                u["path"]!.AsString!, "bromelia-00000000.json", 3, UnitState.Committing, UnitStatus.Success, u["name"]!.AsString!, MediaKind.Movie,
                "bluray", "BD", false, null, "", null, null, null, null, "", 0, files, 1, w.Clock.Now());

            Settle();
            for (int i = 0; i < ops.Count; i++)
            {
                var op = ops[i];
                var (name, arg) = op is JsonValue.String str ? new KeyValuePair<string, JsonValue>(str.Value, JsonValue.Of()) : op.AsObject!.Single();
                switch (name)
                {
                    case "start":
                        OnExecutor(i, arg, async () => { await service.Start(); return JsonValue.Of("ok"); });
                        break;
                    case "check":
                        OnExecutor(i, arg, async () => { await service.Check(); return JsonValue.Of("ok"); });
                        break;
                    case "libraries":
                        w.OnExecutor(() => results[i] = JsonValue.Of(service.Libraries().Select(LibraryDto.Json)));
                        break;
                    case "require":
                        results[i] = Catching(() =>
                        {
                            var h = service.Require(arg.AsString!);
                            return JsonValue.Of(("id", JsonValue.Of(h.Id)), ("path", JsonValue.Of(h.Path)));
                        });
                        break;
                    case "staging":
                        results[i] = Catching(() => JsonValue.Of(service.StagingDirectory(arg["library"]!.AsString!, new Id(arg["job"]!.AsString!))));
                        break;
                    case "blocked":
                    {
                        string? code = null;
                        Task<Lease>? lease = null;
                        var probe = new Id("probe-" + i);
                        w.OnExecutor(() =>
                        {
                            scheduler.Submit(probe, Queue.Processing);
                            lease = scheduler.Acquire(probe, new ResourceSet(Array.Empty<string>(), false, new[] { arg.AsString! }, 0, Array.Empty<string>()),
                                wait => code = wait is Wait.Blocked b ? b.Reason.Code : code, new CancellationSource().Token);
                        });
                        Settle();
                        w.OnExecutor(() =>
                        {
                            if (lease!.IsCompletedSuccessfully) scheduler.Release(Granted(lease));
                            scheduler.Finish(probe);
                        });
                        Settle();
                        results[i] = lease!.IsCompletedSuccessfully ? JsonValue.Null.Instance : JsonValue.Of(code);
                        break;
                    }
                    case "attach":
                        OnExecutor(i, arg, async () => LibraryDto.Json(await service.Attach(arg["id"]!.AsString!, arg["path"]!.AsString!, arg["name"]!.AsString!)));
                        break;
                    case "configure":
                        w.OnExecutor(() =>
                        {
                            config.Value = ConfigBox.Of(arg);
                            bus.Publish(EngineEventType.ConfigChanged, JsonValue.Of(("version", JsonValue.Of(1L))));
                        });
                        Settle();
                        results[i] = JsonValue.Of("ok");
                        break;
                    case "advance":
                        w.Clock.Advance((long)(arg.AsNumber!.Value * 1000));
                        Settle();
                        results[i] = JsonValue.Of("ok");
                        break;
                    case "fs":
                        if (arg["put"] is { } put) foreach (var (p, t) in put.AsObject!) tree.Put(p, t.AsString!);
                        if (arg["moveAway"]?.AsString is { } away) tree.MoveAway(away);
                        results[i] = JsonValue.Of("ok");
                        break;
                    case "held":
                        Settle();
                        results[i] = JsonValue.Of((long)Volatile.Read(ref tree.Held));
                        break;
                    case "release":
                    {
                        tree.Gate.Set();
                        // A woken call counts as held until it leaves the gate: wait for that, or the settle check passes early.
                        var until = DateTime.UtcNow + Patience;
                        while (Volatile.Read(ref tree.Held) > 0)
                        {
                            if (DateTime.UtcNow > until) throw new TimeoutException("the held calls didn't leave");
                            Thread.Sleep(1);
                        }
                        foreach (var d in done) Wait(d);
                        Settle();
                        results[i] = JsonValue.Of("ok");
                        break;
                    }
                    case "commit":
                    case "quarantine":
                    {
                        var u = arg["unit"]!;
                        var it = arg["intent"]!;
                        var fileRows = arg["files"]!.AsArray!;
                        var unit = Unit(u, fileRows.Count);
                        var seq = 0;
                        var intent = new CommitIntent(unit.Id, unit.JobId!.Value, it["staging"]!.AsString!, it["destination"]!.AsString!, it["merge"]!.AsBool!.Value,
                            false, w.Clock.Now(), it["items"]!.AsArray!.Select(x => new CommitItem(seq++, x.AsArray![0].AsString!, x.AsArray![1].AsString!, x.AsArray![2].AsString,
                                x.AsArray![3].AsBool!.Value, false)).ToList());
                        var files = fileRows.Select(f => new UnitFile(f.AsArray![0].AsString!, f.AsArray![1].AsInteger!.Value, f.AsArray![2].AsString!, f.AsArray![3].AsString!)).ToList();
                        var tag = arg["tag"]?.AsString;
                        OffExecutor(i, arg, async () => ResultJson(tag == null ? await service.Commit(unit, intent, files)
                            : await service.Quarantine(unit, intent, files, tag)));
                        break;
                    }
                    case "restart":
                        w.OnExecutor(() => service.Stop());
                        Settle();
                        Build();
                        results[i] = JsonValue.Of("ok");
                        break;
                    case "rollForward":
                        OffExecutor(i, arg, async () =>
                        {
                            var list = new List<JsonValue>();
                            foreach (var intent in await db.OpenIntents())
                            {
                                try { list.Add(ResultJson(await service.RollForward(intent))); }
                                catch (BroFailure f) { list.Add(ErrorJson(f.Error)); }
                            }
                            return JsonValue.Of(list);
                        });
                        break;
                    case "dump":
                        results[i] = Intents(OpenNow(db));
                        break;
                    default:
                        throw new InvalidOperationException("unknown op " + name);
                }
            }
            foreach (var d in done) Wait(d);
            Settle();

            Same(expect["results"]!, JsonValue.Of(results.Select(r => r ?? JsonValue.Null.Instance)), "results");
            JsonValue? got = null;
            w.OnExecutor(() => got = JsonValue.Of(
                ("problems", JsonValue.Of(problems.Problems().Select(ProblemDto.Json))),
                ("events", JsonValue.Of(events.Select(EngineEvent.Json)))));
            if (expect["events"] is { } ev) Same(Sorted(ev), Sorted(got!["events"]!), "events");
            if (expect["problems"] is { } pr) Same(Sorted(pr), Sorted(got!["problems"]!), "problems");
            if (expect["files"] is { } fl)
                Same(Sorted(fl), Sorted(new JsonValue.Object(tree.Files.Select(f => new KeyValuePair<string, JsonValue>(f.Key, JsonValue.Of(Encoding.UTF8.GetString(f.Value)))).ToList())),
                    "files");
            if (expect["modes"] is { } md)
                foreach (var (p, mode) in md.AsObject!) Same(mode.AsInteger, (long?)tree.Modes.GetValueOrDefault(p), "mode of " + p);
            if (expect["dirs"] is { } dr) Same(dr, JsonValue.Of(tree.Dirs.Select(JsonValue.Of)), "dirs");
            if (expect["synced"] is { } sy) Same(sy, JsonValue.Of(tree.Synced.Select(JsonValue.Of)), "synced");
            if (expect["db"] is { } want)
            {
                if (want["libraries"] is { } libs) Same(Sorted(libs), Sorted(JsonValue.Of(db.Libraries.Values.Select(Row))), "db libraries");
                if (want["units"] is { } units)
                    Same(Sorted(units), Sorted(JsonValue.Of(db.Units.Values.Select(u =>
                    {
                        var m = new List<KeyValuePair<string, JsonValue>> { new("id", JsonValue.Of(u.Id.Value)), new("path", JsonValue.Of(u.Path)), new("state", JsonValue.Of(EnumWire.Name(u.State))) };
                        if (u.CommittedAt is { } c) m.Add(new("committedAt", JsonValue.Of(Instant.Format(c))));
                        return (JsonValue)new JsonValue.Object(m);
                    }))), "db units");
                if (want["intents"] is { } intents) Same(intents, Intents(OpenNow(db)), "db intents");
                if (want["unitFiles"] is { } uf)
                    Same(Sorted(uf), Sorted(new JsonValue.Object(db.Files.Select(f => new KeyValuePair<string, JsonValue>(f.Key, FileRows(f.Value))).ToList())), "db unit files");
            }
            return true;
        });
    }

    [Fact]
    public void Sha256HasherHashesInPieces()
    {
        var h = new Sha256Hasher();
        foreach (var piece in new[] { "a", "", "bc" }) h.Update(Encoding.UTF8.GetBytes(piece));
        Assert.Equal("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", h.Finish());
    }
}
