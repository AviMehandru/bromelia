using System.Text;
using Bromelia.Domain;
using Bromelia.Engine;
using Bromelia.Foundation;
using Bromelia.Ports;
using Xunit;
using static Bromelia.Tests.Fixtures;

namespace Bromelia.Scenarios.Tests;

/// <summary>shared/fixtures/engine/local-engine.cases.json: LocalEngine over TestCompositionRoot.</summary>
public sealed class LocalEngineTests
{
    static readonly TimeSpan Patience = TimeSpan.FromSeconds(10);
    const string Seeded = "00000000-0000-4000-8000-1000000000";
    static readonly Id Nobody = new("00000000-0000-4000-8000-0000000000ff");
    const string Marker = "/.bromelia-library";
    static readonly string[] DefaultScan =
    {
        "DRV:0,2,999,12,\"BD-RE HL-DT-ST BD-RE  WH16NS60 1.02 KLAM6E84325\",\"MOVIE_DISC\",\"/dev/rdisk4\"",
        "DRV:1,0,999,0,\"DVD+R-DL MATSHITA DVD-R   UJ8C2 SB00\",\"\",\"/dev/rdisk6\"",
    };

    static JsonValue Obj(params (string, JsonValue)[] members) => JsonValue.Of(members);

    static string MarkerText(string id, string name) =>
        $"{{\n  \"id\": \"m-{id}\",\n  \"name\": \"{name}\",\n  \"created\": \"2026-10-01T12:00:00.000Z\"\n}}\n";

    [Fact]
    public void Cases() => RunCases("engine/local-engine.cases.json", (id, given, expect) => Play(given, expect));

    static bool Play(JsonValue given, JsonValue expect)
    {
        using var run = new Run(given);
        Task.Run(() => run.Steps(expect["steps"]!.AsArray!)).GetAwaiter().GetResult();
        return true;
    }

    sealed class Run : IDisposable
    {
        readonly TestCompositionRoot _r;
        readonly Dictionary<string, Id> _jobs = new();
        readonly Dictionary<string, EventFeed> _feeds = new();
        readonly Dictionary<string, long> _seqs = new();
        (bool ExecutorStopped, int ProcessesRunning)? _released;
        Task? _stopping;

        public Run(JsonValue given)
        {
            var env = new Dictionary<string, string>();
            _r = new TestCompositionRoot(makemkvcon: given["makemkvcon"]?.AsBool ?? true, makemkvconVersion: given["makemkvVersion"]?.AsString,
                mkvmerge: given["mkvmerge"]?.AsBool ?? true, environment: env, streamCapacity: (int)(given["streamCapacity"]?.AsInteger ?? 1024));
            var fs = _r.Fs;
            var config = given["config"] ?? Obj(("version", JsonValue.Of(3)),
                ("libraries", JsonValue.Of(new[] { Obj(("id", JsonValue.Of("lib")), ("name", JsonValue.Of("Library")), ("path", JsonValue.Of("/root/library"))) })));
            fs.Put(TestCompositionRoot.ConfigPath, JsonValue.EncodeCanonical(config));
            if (given["configUnreadable"]?.AsBool == true)
                fs.ReadFails[TestCompositionRoot.ConfigPath] = FsFailed("read", TestCompositionRoot.ConfigPath);
            fs.Put("/root/library" + Marker, Encoding.UTF8.GetBytes(MarkerText("lib", "Library")));
            foreach (var (folder, library) in given["markers"]?.AsObject ?? new List<KeyValuePair<string, JsonValue>>())
                fs.Put(folder + Marker, Encoding.UTF8.GetBytes(MarkerText(library.AsString!, library.AsString!)));
            foreach (var folder in given["folders"]?.AsArray ?? new List<JsonValue>()) fs.PutFolder(folder.AsString!);
            if (given["slowMarkerRead"]?.AsBool == true)
            {
                fs.OnRead = path =>
                {
                    if (path.EndsWith(Marker, StringComparison.Ordinal)) Thread.Sleep(300); // recovery reads it too, before libraryService.start
                };
            }

            _r.Drives.Add(new OsDrive("/dev/disk4", "HL-DT-ST BD-RE WH16NS60", "/Volumes/MOVIE_DISC"));
            _r.Drives.Add(new OsDrive("/dev/disk6", "MATSHITA DVD-R UJ8C2"));
            foreach (var (device, content) in given["content"]?.AsObject ?? new List<KeyValuePair<string, JsonValue>>())
                _r.Drives.SetContent(device, EnumWire.Parse<DiscContent>(content.AsString)!.Value);

            var scan = given["scan"]?.AsArray?.Select(l => l.AsString!).ToList() ?? DefaultScan.ToList();
            var listing = GeneratedListing(Obj(("volume", JsonValue.Of("MOVIE_DISC")), ("type", JsonValue.Of("bluray")),
                ("titles", JsonValue.Of(new[] { Obj(("duration", JsonValue.Of("1:40:00")), ("source", JsonValue.Of(1)), ("chapters", JsonValue.Of(12)),
                    ("size", JsonValue.Of(30_000_000_000))) }))))
                .Split('\n', StringSplitOptions.RemoveEmptyEntries);
            int slowScan = given["slowDriveScan"]?.AsBool == true ? 0 : 1;
            _r.Launcher.LinesFor = spec =>
            {
                if (spec.Arguments.Contains("disc:9999"))
                {
                    if (Interlocked.Exchange(ref slowScan, 1) == 0) Thread.Sleep(300);
                    return scan;
                }
                return Verb(spec) == "info" ? listing : Array.Empty<string>();
            };
            var holds = given["holds"]?.AsArray?.Select(h => h.AsString!).ToHashSet() ?? new HashSet<string>();
            _r.Launcher.RunsUntilStopped = spec => !spec.Arguments.Contains("disc:9999") && Verb(spec) is { } v && holds.Contains(v);
            if (given["stopTakes"]?.AsNumber is { } takes) _r.Launcher.StopTakes = TimeSpan.FromSeconds(takes);
            _r.OnRelease = () => _released = (!_r.Executor.Post(() => { }), _r.Launcher.Running());

            int n = 0;
            var created = new Instant(_r.Clock.Now().UnixMilliseconds - 3_600_000);
            for (int i = 0; i < (given["finishedJobs"]?.AsInteger ?? 0); i++)
                Seed(new Id(Seeded + (++n).ToString("x2")), $"Done {i}", JobState.Finished, null, null, false, new List<JsonValue>(),
                    new Instant(created.UnixMilliseconds + i * 1000));
            foreach (var j in given["jobs"]?.AsArray ?? new List<JsonValue>())
            {
                var id = new Id(Seeded + (++n).ToString("x2"));
                _jobs[j["name"]!.AsString!] = id;
                Seed(id, j["title"]!.AsString!, EnumWire.Parse<JobState>(j["state"]!.AsString)!.Value, j["driveId"]?.AsString, j["mediaGeneration"]?.AsInteger,
                    j["automatic"]?.AsBool == true, j["rows"]!.AsArray!, new Instant(created.UnixMilliseconds + 500_000 + n * 1000));
            }
        }

        static BroError FsFailed(string op, string path) =>
            new("fs.failed", ("operation", JsonValue.Of(op)), ("path", JsonValue.Of(path)), ("reason", JsonValue.Of("injected")));

        /// <summary>The makemkvcon command of a run: info, mkv or backup.</summary>
        static string? Verb(ProcessSpec spec) => spec.Arguments.FirstOrDefault(a => a is "info" or "mkv" or "backup");

        void Seed(Id id, string title, JobState state, string? driveId, long? generation, bool automatic, IReadOnlyList<JsonValue> rows, Instant created)
        {
            var request = new JobRequest(JobKind.VideoDisc, title, automatic, driveId, generation);
            bool finished = state == JobState.Finished;
            var record = new JobRecord(id, JobKind.VideoDisc, null, state, finished ? Outcome.Succeeded : null, null, null, null, Queue.Acquisition, 0, driveId,
                generation, automatic, null, title, null, request, null, null, created, FinishedAt: finished ? created : null);
            Wait(_r.Store.Jobs().Insert(record));
            int seq = 0;
            foreach (var row in rows)
            {
                var kind = EnumWire.Parse<StepKind>(row["kind"]!.AsString)!.Value;
                var rowState = EnumWire.Parse<StepState>(row["state"]!.AsString)!.Value;
                Wait(_r.Store.Steps().Save(new StepRecord(id, seq++, kind, rowState, 1, created,
                    rowState == StepState.Running ? null : created, rowState == StepState.Succeeded ? new StepOutput(kind, Obj()) : null)));
            }
        }

        static void Wait(Task task) => task.GetAwaiter().GetResult();

        public void Dispose()
        {
            foreach (var f in _feeds.Values) f.Close();
            _r.Dispose();
        }

        // ---- steps ----

        public async Task Steps(IReadOnlyList<JsonValue> steps)
        {
            int i = 0;
            foreach (var step in steps)
            {
                var where = $"step {i++} {C(step)}";
                try
                {
                    await Step(step);
                }
                catch (Exception e) when (e is not CaseFailure)
                {
                    throw new CaseFailure($"{where}: {e.GetType().Name}: {e.Message}");
                }
                catch (CaseFailure e)
                {
                    throw new CaseFailure($"{where}: {e.Message}");
                }
            }
        }

        async Task Step(JsonValue step)
        {
            if (step["call"]?.AsString is { } op)
            {
                await Call(op, step);
                return;
            }
            var (key, value) = step.AsObject!.First();
            switch (key)
            {
                case "start":
                    await Expect(step, async () =>
                    {
                        await _r.Start();
                        return null;
                    });
                    break;
                case "stop":
                    await _r.Stop();
                    break;
                case "stopBegins":
                    _stopping = _r.Stop();
                    await Task.Delay(50);
                    break;
                case "stopEnds":
                    await Within(Done(_stopping!), "stop");
                    break;
                case "monitoring":
                    if (_r.Drives.Monitoring() != value.AsBool) throw new CaseFailure("monitoring " + _r.Drives.Monitoring());
                    break;
                case "timers":
                    if (_r.Clock.Pending() != value.AsInteger) throw new CaseFailure("timers pending " + _r.Clock.Pending());
                    break;
                case "restart":
                    await _r.Restart();
                    break;
                case "settle":
                    await Settle();
                    break;
                case "engineLog":
                    Same(value, JsonValue.Of(_r.Log.Lines().Select(Line)), "engine log", exact: true);
                    break;
                case "processes":
                    Same(value, JsonValue.Of(_r.Launcher.Specs().Where(s => !s.Arguments.Contains("disc:9999"))
                        .Select(s => JsonValue.Of(Verb(s) + " " + s.Arguments.FirstOrDefault(a => a.StartsWith("disc:") || a.StartsWith("iso:") || a.StartsWith("file:"))))),
                        "processes", exact: true);
                    break;
                case "trayCalls":
                    Same(value, JsonValue.Of(_r.Drives.Calls().Select(c => JsonValue.Of(c))), "tray calls", exact: true);
                    break;
                case "files":
                    foreach (var p in value["exist"]?.AsArray ?? new List<JsonValue>())
                        if (!_r.Fs.Exists(Subst(p.AsString!))) throw new CaseFailure("missing " + Subst(p.AsString!));
                    foreach (var p in value["absent"]?.AsArray ?? new List<JsonValue>())
                        if (_r.Fs.Exists(Subst(p.AsString!))) throw new CaseFailure("there " + Subst(p.AsString!));
                    break;
                case "stored":
                    foreach (var (name, want) in value.AsObject!)
                    {
                        var record = await _r.Store.Jobs().Load(_jobs[name]) ?? throw new CaseFailure("no record of " + name);
                        var r = record.Request;
                        var request = Obj(("kind", JsonValue.Of(EnumWire.Name(r.Kind))), ("title", JsonValue.Of(r.Title)), ("automatic", JsonValue.Of(r.Automatic)),
                            ("driveId", JsonValue.Of(r.DriveId)), ("mediaGeneration", r.MediaGeneration is { } g ? JsonValue.Of(g) : JsonValue.Null.Instance),
                            ("options", r.Options ?? JsonValue.Null.Instance));
                        Same(want, Obj(("mediaGeneration", record.MediaGeneration is { } m ? JsonValue.Of(m) : JsonValue.Null.Instance), ("request", request)),
                            "stored " + name);
                    }
                    break;
                case "released":
                    if (_released is not { } released) throw new CaseFailure("release never ran");
                    Same(value, Obj(("executorStopped", JsonValue.Of(released.ExecutorStopped)), ("processesRunning", JsonValue.Of(released.ProcessesRunning))),
                        "released");
                    break;
                case "feed":
                {
                    long? after = value["after"] switch
                    {
                        JsonValue.String s => long.Parse(Subst(s.Value)),
                        JsonValue.Integer a => a.Value,
                        _ => null,
                    };
                    var jobLog = value["jobLog"]?.AsArray?.Select(j => _jobs[j.AsString!]).ToList() ?? new List<Id>();
                    _feeds[step["as"]!.AsString!] = await _r.Engine.Events(after, jobLog);
                    break;
                }
                case "next":
                {
                    var feed = _feeds[value.AsString!];
                    foreach (var want in step["expect"]!.AsArray!)
                    {
                        var e = await Within(feed.Next(), "the next event");
                        if (want is JsonValue.Null)
                        {
                            if (e != null) throw new CaseFailure("an event after the feed closed: " + EngineEvent.Json(e));
                            continue;
                        }
                        if (e == null) throw new CaseFailure("the feed ended");
                        var json = EngineEvent.Json(e);
                        var members = want.AsObject!.Where(m => m.Key is not ("seqIsLast" or "jobCount" or "firstJob")).ToList();
                        Same(Subst(new JsonValue.Object(members)), json, "event");
                        if (want["seqIsLast"]?.AsBool == true && e.Seq != _r.Bus.LastSeq)
                            throw new CaseFailure($"event seq {e.Seq}, last {_r.Bus.LastSeq}");
                        var jobs = json["data"]?["jobs"]?.AsArray;
                        if (want["jobCount"]?.AsInteger is { } count && jobs?.Count != count)
                            throw new CaseFailure($"snapshot jobs {jobs?.Count}, wanted {count}");
                        if (want["firstJob"]?.AsString is { } first && jobs?[0]["id"]?.AsString != _jobs[first].Value)
                            throw new CaseFailure($"first snapshot job {jobs?[0]["id"]}");
                    }
                    break;
                }
                case "drain":
                {
                    var feed = _feeds[value.AsString!];
                    var logsOf = new SortedSet<string>(StringComparer.Ordinal);
                    while (true)
                    {
                        var next = feed.Next();
                        if (await Task.WhenAny(next, Task.Delay(300)) != next) break;
                        if (await next is not { } e) break;
                        if (e.Type == EngineEventType.JobLog) logsOf.Add(e.Data["jobId"]!.AsString!);
                    }
                    var want = new SortedSet<string>(step["jobLogsOf"]!.AsArray!.Select(j => _jobs[j.AsString!].Value), StringComparer.Ordinal);
                    if (!want.SetEquals(logsOf)) throw new CaseFailure("jobLog events of " + string.Join(", ", logsOf));
                    break;
                }
                case "seq":
                    _seqs[value.AsString!] = _r.Bus.LastSeq;
                    break;
                case "fail":
                {
                    var path = value["path"]!.AsString!;
                    var fop = value["op"]!.AsString!;
                    _r.Fs.Fails.Add((fop, path, FsFailed(fop, path)));
                    break;
                }
                case "unfail":
                    _r.Fs.Fails.Clear();
                    break;
                case "unmount":
                    await _r.Unmount(value.AsString!);
                    break;
                case "mount":
                    await _r.Mount(value.AsString!);
                    break;
                case "limitsOf":
                {
                    var limits = SchedulerLimits.Of(ConfigCodec.Decode(JsonValue.EncodeCanonical(value["config"]!)), (int)value["cores"]!.AsInteger!.Value);
                    Same(step["expect"]!, Obj(("acquisitions", JsonValue.Of(limits.Acquisitions)), ("processing", JsonValue.Of(limits.Processing)),
                        ("cpu", JsonValue.Of(limits.Cpu)), ("ioPerVolume", JsonValue.Of(limits.IoPerVolume))), "limits", exact: true);
                    break;
                }
                case "systemSeqIsLast":
                {
                    var status = await _r.Engine.System();
                    if (status.EventSeq != _r.Bus.LastSeq) throw new CaseFailure($"system eventSeq {status.EventSeq}, last {_r.Bus.LastSeq}");
                    break;
                }
                default:
                    throw new CaseFailure("unknown step " + key);
            }
        }

        static JsonValue ErrorJson(BroError e) => e.Params.Members.Count > 0
            ? Obj(("code", JsonValue.Of(e.Code)), ("params", e.Params))
            : Obj(("code", JsonValue.Of(e.Code)));

        static JsonValue Line(BroMessage m)
        {
            var members = new List<(string, JsonValue)> { ("code", JsonValue.Of(MessageCode.Wire(m.Code))) };
            if (m.Params.Members.Count > 0) members.Add(("params", m.Params));
            return JsonValue.Of(members.ToArray());
        }

        Id Job(JsonValue name) => _jobs.TryGetValue(name.AsString!, out var id) ? id : Nobody;

        async Task Call(string op, JsonValue step)
        {
            var a = step["args"]?.AsArray ?? new List<JsonValue>();
            var e = _r.Engine;
            var result = await Expect(step, async () => op switch
            {
                "system" => SystemStatusDto.Json(await e.System()),
                "stop" => await Done(e.Stop()),
                "drives" => JsonValue.Of((await e.Drives()).Select(DriveDto.Json)),
                "eject" => await Done(e.Eject(a[0].AsString!)),
                "closeTray" => await Done(e.CloseTray(a[0].AsString!)),
                "closeAllTrays" => await Done(e.CloseAllTrays()),
                "rip" => JobDto.Json(await e.Rip(a[0].AsString!, Options(a[1]))),
                "jobs" => JobPageDto.Json(await e.Jobs(new JobQueryDto(Array.Empty<JobState>(), Array.Empty<Outcome>()))),
                "job" => JobDetailDto.Json(await e.Job(Job(a[0]))),
                "jobLog" => JsonValue.Of((await e.JobLog(Job(a[0]), a[1].AsInteger!.Value)).Select(LogLineDto.Json)),
                "cancel" => await Done(e.Cancel(Job(a[0]))),
                "retry" => JobDto.Json(await e.Retry(Job(a[0]), a[1].AsString is { } k ? EnumWire.Parse<StepKind>(k) : null)),
                "keepApart" => await Done(e.KeepApart(Job(a[0]))),
                "decide" => await Done(e.Decide(Job(a[0]), a[1].AsString!, a[2].AsString!)),
                "move" => await Done(e.Move(Job(a[0]), (int)a[1].AsInteger!.Value)),
                "startNow" => await Done(e.StartNow(Job(a[0]))),
                "libraries" => JsonValue.Of((await e.Libraries()).Select(LibraryDto.Json)),
                "attachLibrary" => LibraryDto.Json(await e.AttachLibrary(a[0].AsString!, a[1].AsString!)),
                "config" => ConfigEnvelope.Json(await e.Config()),
                "updateConfig" => ConfigEnvelope.Json(await e.UpdateConfig(a[0], a[1].AsInteger!.Value)),
                "validateConfig" => JsonValue.Of((await e.ValidateConfig(a[0])).Select(i => i.ToJson())),
                "setSecret" => await Done(e.SetSecret(a[0].AsString!, a[1].AsString!)),
                "exportConfig" => await e.ExportConfig(),
                "startJob" => JobDto.Json(await e.StartJob(Request(a[0]))),
                _ => throw new CaseFailure("unknown operation " + op),
            });
            if (step["as"]?.AsString is { } name && result?["id"]?.AsString is { } id) _jobs[name] = new Id(id);
            if (step["includes"]?.AsArray is { } includes)
                foreach (var want in includes)
                    if (!result!.AsArray!.Any(got => Matches(Subst(want), got)))
                        throw new CaseFailure($"no line like {want} in {result}");
        }

        static async Task<JsonValue?> Done(Task task)
        {
            await task;
            return null;
        }

        static RipOptionsDto Options(JsonValue o) => new(EnumWire.Parse<RipMode>(o["mode"]?.AsString), o["profile"]?.AsString, o["library"]?.AsString,
            o["titles"]?.AsArray?.Select(t => (int)t.AsInteger!.Value).ToList(), o["outputFolder"]?.AsString);

        static JobRequest Request(JsonValue r) => new(EnumWire.Parse<JobKind>(r["kind"]!.AsString)!.Value, r["title"]!.AsString!, r["automatic"]?.AsBool == true,
            r["driveId"]?.AsString, r["mediaGeneration"]?.AsInteger, Options: r["options"], Details: r["details"]);

        /// <summary>Runs the action; checks its result against expect, or its failure against error.</summary>
        async Task<JsonValue?> Expect(JsonValue step, Func<Task<JsonValue?>> action)
        {
            JsonValue? result;
            try
            {
                result = await Within(action(), "the call");
            }
            catch (BroFailure f)
            {
                if (step["error"] is not { } wantError) throw new CaseFailure("failed: " + C(ErrorJson(f.Error)));
                Same(Subst(wantError), ErrorJson(f.Error), "error");
                return null;
            }
            if (step["error"] is { } error) throw new CaseFailure($"no error, wanted {C(error)}; got {C(result)}");
            if (step["expect"] is { } want and not JsonValue.Null) Same(Subst(want), result ?? JsonValue.Null.Instance, "result");
            return result;
        }

        static async Task<T> Within<T>(Task<T> task, string what)
        {
            if (await Task.WhenAny(task, Task.Delay(Patience)) != task) throw new CaseFailure(what + " never ended");
            return await task;
        }

        /// <summary>Until the executor's activity is the held processes' (each one a running step's worker), seen three times
        /// in a row.</summary>
        async Task Settle()
        {
            var deadline = DateTime.UtcNow + Patience;
            int seen = 0;
            while (seen < 3)
            {
                if (DateTime.UtcNow > deadline)
                    throw new CaseFailure($"never settled: activity {_r.Executor.Activity}, held {_r.Launcher.Held()}");
                await Task.Delay(5);
                seen = _r.Executor.Activity == _r.Launcher.Held() ? seen + 1 : 0;
            }
        }

        // ---- matching ----

        JsonValue Subst(JsonValue v) => v switch
        {
            JsonValue.String s => JsonValue.Of(Subst(s.Value)),
            JsonValue.Array a => JsonValue.Of(a.Items.Select(Subst)),
            JsonValue.Object o => new JsonValue.Object(o.Members.Select(m => new KeyValuePair<string, JsonValue>(m.Key, Subst(m.Value))).ToList()),
            _ => v,
        };

        string Subst(string s)
        {
            foreach (var (name, id) in _jobs) s = s.Replace("{" + name + "}", id.Value);
            foreach (var (name, seq) in _seqs) s = s.Replace("{" + name + "}", seq.ToString());
            return s;
        }

        /// <summary>The JSON on one line (failure messages).</summary>
        static string C(JsonValue? v) => v == null ? "null" : System.Text.RegularExpressions.Regex.Replace(v.ToString(), @"\s*\n\s*", " ");

        static void Same(JsonValue want, JsonValue got, string what, bool exact = false)
        {
            if (!Matches(want, got, exact)) throw new CaseFailure($"{what}: wanted {C(want)}\n got {C(got)}");
        }

        /// <summary>Objects: every wanted member matches (a wanted null: absent or null); exact also wants no others.
        /// Arrays: the same length, element by element. Anything else: equal.</summary>
        static bool Matches(JsonValue want, JsonValue? got, bool exact = false)
        {
            switch (want)
            {
                case JsonValue.Null:
                    return got is null or JsonValue.Null;
                case JsonValue.Object o:
                    if (got is not JsonValue.Object g) return false;
                    if (exact && g.Members.Count != o.Members.Count) return false;
                    return o.Members.All(m => Matches(m.Value, got[m.Key], exact));
                case JsonValue.Array a:
                    return got is JsonValue.Array ga && ga.Items.Count == a.Items.Count && a.Items.Zip(ga.Items).All(p => Matches(p.First, p.Second, exact));
                case JsonValue.Integer or JsonValue.Number:
                    return got?.AsNumber is { } n && want.AsNumber == n;
                default:
                    return Equals(want, got);
            }
        }
    }

    sealed class CaseFailure : Exception
    {
        public CaseFailure(string message) : base(message) { }
    }
}
