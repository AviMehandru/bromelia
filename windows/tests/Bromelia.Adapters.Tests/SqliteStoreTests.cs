using System.Text;
using Bromelia.Domain;
using Bromelia.Foundation;
using Bromelia.Ports;
using Microsoft.Data.Sqlite;
using Xunit;
using static Bromelia.Tests.Fixtures;
using CancellationToken = Bromelia.Foundation.CancellationToken;

namespace Bromelia.Adapters.Tests;

/// <summary>A clock stopped at 2026-10-04T12:00:00.000Z.</summary>
internal sealed class FixedClock : IClock
{
    public Instant At = Instant.Parse("2026-10-04T12:00:00.000Z")!.Value;
    public Instant Now() => At;
    public Duration Monotonic() => new(0);
    public Task Sleep(Duration duration, CancellationToken cancel) => Task.CompletedTask;
    public ITimerHandle Timer(TimerSchedule schedule, Action handler) => throw new NotSupportedException();
}

/// <summary>shared/fixtures/adapters/store.cases.json, and every record type read back equal.</summary>
public sealed class SqliteStoreTests : IDisposable
{
    readonly string _dir = Path.Combine(Path.GetTempPath(), "bromelia-store-" + Guid.NewGuid().ToString("N"));
    readonly List<SqliteStore> _open = new();

    public SqliteStoreTests() => Directory.CreateDirectory(_dir);

    public void Dispose()
    {
        foreach (var s in _open) s.Close();
        SqliteConnection.ClearAllPools();
        try { Directory.Delete(_dir, true); } catch (IOException) { }
    }

    SqliteStore NewStore(string name = "db")
    {
        var store = SqliteStore.Open(Path.Combine(_dir, name + ".sqlite"), new FixedClock());
        _open.Add(store);
        store.Migrate().GetAwaiter().GetResult();
        return store;
    }

    void Seed(string name, JsonValue rows)
    {
        using var db = new SqliteConnection($"Data Source={Path.Combine(_dir, name + ".sqlite")};Pooling=False");
        db.Open();
        using (var fk = db.CreateCommand()) { fk.CommandText = "PRAGMA foreign_keys=ON"; fk.ExecuteNonQuery(); }
        foreach (var table in rows.AsObject!)
            foreach (var row in table.Value.AsArray!)
            {
                var cols = row.AsObject!;
                using var c = db.CreateCommand();
                c.CommandText = $"INSERT INTO {table.Key} ({string.Join(", ", cols.Select(m => m.Key))}) VALUES ({string.Join(", ", cols.Select((_, i) => "$p" + i))})";
                for (int i = 0; i < cols.Count; i++)
                {
                    object value = cols[i].Value switch
                    {
                        JsonValue.Null => DBNull.Value,
                        JsonValue.String s when table.Key == "lookup_cache" && cols[i].Key == "response" => Encoding.UTF8.GetBytes(s.Value),
                        JsonValue.String s => s.Value,
                        JsonValue.Integer n => n.Value,
                        JsonValue.Number d => d.Value,
                        JsonValue.Bool b => b.Value ? 1L : 0L,
                        _ => throw new InvalidOperationException(cols[i].Key),
                    };
                    c.Parameters.AddWithValue("$p" + i, value);
                }
                c.ExecuteNonQuery();
            }
    }

    static JsonValue Ids<T>(IEnumerable<T> items, Func<T, string> id) => JsonValue.Of(items.Select(x => JsonValue.Of(id(x))));

    static ContinuationQuery Continuation(JsonValue a) => new(a["name"]!.AsString!, a["labelTitle"]!.AsString!, (int?)a["season"]?.AsInteger,
        (int?)a["part"]?.AsInteger, (int?)a["volume"]?.AsInteger, (int)a["disc"]!.AsInteger!.Value);

    [Fact]
    public void TheSharedCasesPass()
    {
        int n = 0;
        RunCases("adapters/store.cases.json", (id, given, expect) =>
        {
            var name = "case" + n++;
            var store = NewStore(name);
            Seed(name, given["rows"]!);
            var a = given["args"] ?? JsonValue.Of();
            void Same(string key, JsonValue actual) => Assert.Equal(expect[key], actual);
            switch (given["call"]!.AsString)
            {
                case "jobs.active": Same("ids", Ids(store.Jobs().Active().Result, j => j.Id.Value)); break;
                case "jobs.query":
                    Same("ids", Ids(store.Jobs().Query(new JobQuery(EnumWire.Parse<JobState>(a["state"]?.AsString), EnumWire.Parse<JobKind>(a["kind"]?.AsString),
                        a["finishedAfter"]?.AsString is { } f ? Instant.Parse(f) : null, (int)a["limit"]!.AsInteger!.Value, (int)a["offset"]!.AsInteger!.Value)).Result,
                        j => j.Id.Value));
                    break;
                case "steps.completed": Same("seqs", JsonValue.Of(store.Steps().Completed(new Id(a["jobId"]!.AsString!)).Result.Select(s => JsonValue.Of(s.Seq)))); break;
                case "steps.load": Same("seqs", JsonValue.Of(store.Steps().Load(new Id(a["jobId"]!.AsString!)).Result.Select(s => JsonValue.Of(s.Seq)))); break;
                case "units.query":
                    Same("ids", Ids(store.Units().Query(new UnitQuery(a["libraryId"]?.AsString, a["text"]?.AsString, EnumWire.Parse<UnitState>(a["state"]?.AsString),
                        (int)a["limit"]!.AsInteger!.Value, (int)a["offset"]!.AsInteger!.Value)).Result, u => u.Id.Value));
                    break;
                case "units.leastRecentlyVerified": Same("ids", Ids(store.Units().LeastRecentlyVerified((int)a["limit"]!.AsInteger!.Value).Result, u => u.Id.Value)); break;
                case "units.openIntents":
                    Same("intents", JsonValue.Of(store.Units().OpenIntents().Result.Select(i => JsonValue.Of(new[]
                    {
                        JsonValue.Of(i.UnitId.Value),
                        JsonValue.Of(i.Items.Select(x => JsonValue.Of($"{x.Seq} {x.FromPath} {x.ToPath}{(x.IsDir ? " dir" : "")}{(x.Moved ? " moved" : "")}"))),
                    }))));
                    break;
                case "catalog.archivedBefore": Same("ids", Ids(store.Catalog().ArchivedBefore(a["fingerprint"]!.AsString!).Result, u => u.Id.Value)); break;
                case "catalog.previousDisc":
                    var prev = store.Catalog().PreviousDisc(Continuation(a)).Result;
                    Same("previous", prev is null ? JsonValue.Null.Instance : JsonValue.Of(("name", JsonValue.Of(prev.Name)), ("disc", JsonValue.Of(prev.Disc!.Value)),
                        ("lastEpisode", JsonValue.Of(prev.LastEpisode!.Value)), ("folder", JsonValue.Of(prev.Folder))));
                    break;
                case "catalog.highestEpisode":
                    var e = store.Catalog().HighestEpisode(Continuation(a)).Result;
                    Same("episode", e is { } v ? JsonValue.Of(v) : JsonValue.Null.Instance);
                    break;
                case "catalog.works": Same("ids", Ids(store.Catalog().Works(a["query"]!.AsString!).Result, w => w.Id.Value)); break;
                case "catalog.set":
                    var set = store.Catalog().Set(new Id(a["setId"]!.AsString!)).Result!;
                    JsonValue N(int? x) => x is { } i ? JsonValue.Of(i) : JsonValue.Null.Instance;
                    Same("set", JsonValue.Of(("workId", JsonValue.Of(set.WorkId?.Value)), ("labelTitle", JsonValue.Of(set.LabelTitle)), ("season", N(set.Season)),
                        ("part", N(set.Part)), ("volume", N(set.Volume)), ("description", JsonValue.Of(set.Description)), ("knownCount", N(set.KnownCount))));
                    break;
                case "checks.forUnit":
                    var checks = store.Checks().ForUnit(new Id(a["unitId"]!.AsString!)).Result;
                    Same("ids", Ids(checks, c => c.Id.Value));
                    Same("changed", JsonValue.Of(checks[0].Changed.Select(JsonValue.Of)));
                    Assert.Equal(expect["latest"]!.AsString, store.Checks().Latest(expect["latestFolder"]!.AsString!).Result!.Id.Value);
                    break;
                case "replicas.lagging": Same("ids", Ids(store.Replicas().Lagging().Result, r => r.Id.Value)); break;
                case "outbox.due": Same("ids", Ids(store.Outbox().Due(Instant.Parse(a["now"]!.AsString!)!.Value).Result, o => o.Id.Value)); break;
                case "lookupCache.get":
                    Same("bodies", JsonValue.Of(a["keys"]!.AsArray!.Select(k => store.LookupCache().Get(a["provider"]!.AsString!, k.AsString!).Result is { } r
                        ? JsonValue.Of(Encoding.UTF8.GetString(r.Body)) : JsonValue.Null.Instance)));
                    break;
                case "drives.recordStats":
                    DriveStats Stats(JsonValue s) => new((int)s["jobs"]!.AsInteger!.Value, (int)s["failedJobs"]!.AsInteger!.Value, (int)s["readErrorJobs"]!.AsInteger!.Value,
                        (int)s["readErrors"]!.AsInteger!.Value, s["bytesRead"]!.AsInteger!.Value, s["secondsReading"]!.AsNumber!.Value);
                    store.Drives().RecordStats(a["driveId"]!.AsString!, a["day"]!.AsString!, Stats(a["twice"]!)).Wait();
                    store.Drives().RecordStats(a["driveId"]!.AsString!, a["day"]!.AsString!, Stats(a["then"]!)).Wait();
                    using (var db = new SqliteConnection($"Data Source={Path.Combine(_dir, name + ".sqlite")};Pooling=False"))
                    {
                        db.Open();
                        using var c = db.CreateCommand();
                        c.CommandText = "SELECT jobs, failed_jobs, read_error_jobs, read_errors, bytes_read, seconds_reading FROM drive_stats_daily";
                        using var r = c.ExecuteReader();
                        Assert.True(r.Read());
                        Same("row", JsonValue.Of(("jobs", JsonValue.Of(r.GetInt64(0))), ("failed_jobs", JsonValue.Of(r.GetInt64(1))),
                            ("read_error_jobs", JsonValue.Of(r.GetInt64(2))), ("read_errors", JsonValue.Of(r.GetInt64(3))), ("bytes_read", JsonValue.Of(r.GetInt64(4))),
                            ("seconds_reading", new JsonValue.Number(r.GetDouble(5)))));
                    }
                    break;
                default: return false;
            }
            return true;
        });
    }

    static readonly Instant T1 = Instant.Parse("2026-10-01T12:00:00.000Z")!.Value;
    static readonly Instant T2 = Instant.Parse("2026-10-02T12:00:00.123Z")!.Value;
    static Id U(int n) => new($"00000000-0000-4000-8000-{n:D12}");
    static JsonValue.Object P(params (string, JsonValue)[] m) => (JsonValue.Object)JsonValue.Of(m);

    [Fact]
    public async Task EveryRecordReadsBackEqual()
    {
        var store = NewStore();
        var job = new JobRecord(U(1), JobKind.VideoDisc, null, JobState.Running, Outcome.Failed,
            new BroError("process.couldNotStart", P(("tool", JsonValue.Of("makemkvcon"))), new BroError("fs.notFound", P(("path", JsonValue.Of("/x"))))),
            JsonValue.Of(("drive", JsonValue.Of("d1"))), null, Queue.Acquisition, 2, "d1", 3, true, "mkv", "Disc", "v1:aa",
            new JobRequest(JobKind.VideoDisc, "Disc", true, "d1", 3, U(9), null, null, JsonValue.Of(("x", JsonValue.Of(1L))), null),
            new JobPlan(new[] { StepKind.AwaitMedia, StepKind.Probe }, "mkv", "default", "lib", null), null, T1, T2);
        await store.Jobs().Insert(job);
        Assert.Equal(J(job), J((await store.Jobs().Load(U(1)))!));
        var updated = job with { State = JobState.Finished, FinishedAt = T2, Plan = null, Error = null };
        await store.Jobs().Update(updated);
        Assert.Equal(J(updated), J((await store.Jobs().Load(U(1)))!));
        Assert.Null(await store.Jobs().Load(U(2)));

        var step = new StepRecord(U(1), 0, StepKind.Probe, StepState.Succeeded, 2, T1, T2,
            new StepOutput(StepKind.Probe, JsonValue.Of(("titles", JsonValue.Of(3L))), new BroMessage(MessageCode.JobCancelled, Severity.Warning)),
            new BroError("job.cancelled"), JsonValue.Of(("sector", JsonValue.Of(10L))));
        await store.Steps().Save(step);
        await store.Steps().Save(step with { Attempt = 3 });
        Assert.Equal(J(step with { Attempt = 3 }), J((await store.Steps().Load(U(1))).Single()));

        Sql("db", "INSERT INTO libraries (id, name, path, marker_id, attached_at) VALUES ('lib', 'L', '/archive', 'm', '2026-10-01T12:00:00.000Z')");
        var unit = new UnitRecord(U(100), "lib", null, U(1), "Movies/Disc", "bromelia-00000000.json", 3, UnitState.Committing, UnitStatus.Success, "Disc",
            MediaKind.Movie, "bluray", "BD", true, "v1:aa", "DISC", null, null, null, 1, "v2.0", 1234, 2, 1, T1);
        var intent = new CommitIntent(U(100), U(1), "/staging", "/archive/Movies/Disc", false, false, T1,
            new[] { new CommitItem(0, "a.mkv", "a.mkv", new string('a', 64), false, false), new CommitItem(1, "extras", "extras", null, true, false) });
        await store.Units().BeginCommit(unit, intent);
        await store.Units().MarkMoved(U(100), 1);
        var open = (await store.Units().OpenIntents()).Single();
        Assert.Equal(intent.UnitId, open.UnitId);
        Assert.Equal(new[] { false, true }, open.Items.Select(i => i.Moved));
        var committed = unit with { State = UnitState.Committed, CommittedAt = T2 };
        await store.Units().FinishCommit(intent, committed, new[] { new UnitFile("a.mkv", 1, new string('a', 64), "title", 0, null) });
        Assert.Empty(await store.Units().OpenIntents());
        Assert.Equal(committed, await store.Units().Get(U(100)));
        await store.Units().MarkMissing(U(100));
        Assert.Equal(UnitState.Missing, (await store.Units().Get(U(100)))!.State);

        var check = new CheckRecord(U(200), U(100), null, "/archive/Movies/Disc", U(1), T1, T2, CheckResult.Damaged, 2, 1234,
            new[] { "a.mkv" }, Array.Empty<string>(), new[] { "b.mkv" }, Array.Empty<string>(), new BroMessage(MessageCode.FsFailed, Severity.Error, ("path", JsonValue.Of("/a"))));
        await store.Checks().Insert(check);
        Assert.Equal(J(check), J((await store.Checks().ForUnit(U(100))).Single()));

        var replica = new ReplicaRecord(U(300), U(100), "nas", "/nas/Disc", ReplicaState.Verified, T2);
        await store.Replicas().Upsert(replica with { State = ReplicaState.Copying, VerifiedAt = null });
        await store.Replicas().Upsert(replica);
        Assert.Equal(replica, (await store.Replicas().ForUnit(U(100))).Single());

        var drive = new DriveRecord("d1", "BD-RE X", "X", "/dev/rdisk4", "left", T1, T1);
        await store.Drives().Upsert(drive);
        await store.Drives().Upsert(drive with { LastDevice = "/dev/rdisk5", LastSeenAt = T2, FirstSeenAt = T2 });
        Assert.Equal(drive with { LastDevice = "/dev/rdisk5", LastSeenAt = T2 }, (await store.Drives().All()).Single());

        await store.LookupCache().Put("tmdb", "k", new HttpResponse(200, new List<KeyValuePair<string, string>>(), new byte[] { 1, 2, 3 }),
            Instant.Parse("2026-10-05T00:00:00.000Z")!.Value);
        Assert.Equal(new byte[] { 1, 2, 3 }, (await store.LookupCache().Get("tmdb", "k"))!.Body);

        var note = new OutboxEntry(U(400), "t", U(1), new BroMessage(MessageCode.NotifyTestTitle),
            new[] { new BroMessage(MessageCode.NotifyTestBody, Severity.Info, ("target", JsonValue.Of("t"))) }, StatusWord.Success, T1, 0, T1);
        await store.Outbox().Add(note);
        Assert.Equal(J(note), J((await store.Outbox().Due(T2)).Single()));
        await store.Outbox().MarkFailed(U(400), new BroError("http.failed"), null);
        Assert.Empty(await store.Outbox().Due(T2));
        await store.Outbox().MarkSent(U(400));

        await store.Kv().Set("a", JsonValue.Of(("b", JsonValue.Of(true))));
        await store.Kv().Set("a", JsonValue.Of(("b", JsonValue.Of(false))));
        Assert.Equal(JsonValue.Of(("b", JsonValue.Of(false))), await store.Kv().Get("a"));
        Assert.Null(await store.Kv().Get("nope"));
    }

    [Fact]
    public async Task ATransactionCommitsOrRollsBackAsAWhole()
    {
        var store = NewStore();
        await store.Transaction(async tx =>
        {
            await tx.Kv().Set("a", JsonValue.Of(1L));
            await tx.Kv().Set("b", JsonValue.Of(2L));
            return 0;
        });
        await Assert.ThrowsAsync<InvalidOperationException>(() => store.Transaction<int>(async tx =>
        {
            await tx.Kv().Set("a", JsonValue.Of(10L));
            throw new InvalidOperationException("boom");
        }));
        Assert.Equal(JsonValue.Of(1L), await store.Kv().Get("a"));
        Assert.Equal(JsonValue.Of(2L), await store.Kv().Get("b"));
    }

    [Fact]
    public async Task TheSchemaIsTheSharedOneAndMigratingTwiceChangesNothing()
    {
        var store = NewStore();
        await store.Migrate();
        using var db = new SqliteConnection($"Data Source={Path.Combine(_dir, "db.sqlite")};Pooling=False");
        db.Open();
        string One(string sql) { using var c = db.CreateCommand(); c.CommandText = sql; return Convert.ToString(c.ExecuteScalar())!; }
        Assert.Equal("1", One("PRAGMA user_version"));
        Assert.Equal("wal", One("PRAGMA journal_mode"));
        Assert.Equal(Text("../schema/db/0001_init.expected.txt"), SchemaDump(db));
        // A unit in a library the database doesn't have breaks a foreign key: store.failed, and nothing is written.
        var orphan = new UnitRecord(U(100), "nope", null, null, "p", "r", 3, UnitState.Committing, UnitStatus.Success, "n", MediaKind.Movie, "dvd", "DVD",
            false, null, "", null, null, null, null, "", 0, 0, 1, T1);
        var e = await Assert.ThrowsAsync<BroFailure>(() => store.Units().BeginCommit(orphan,
            new CommitIntent(U(100), U(1), "/s", "/d", false, false, T1, Array.Empty<CommitItem>())));
        Assert.Equal("store.failed", e.Error.Code);
        Assert.Null(await store.Units().Get(U(100)));
    }

    /// <summary>A record's full contents as JSON (message codes by their wire names), to compare records holding
    /// lists.</summary>
    static string J(object record) => System.Text.Json.JsonSerializer.Serialize(record, record.GetType(),
        new System.Text.Json.JsonSerializerOptions { Converters = { new CodeConverter() } });

    sealed class CodeConverter : System.Text.Json.Serialization.JsonConverter<MessageCode>
    {
        public override MessageCode Read(ref System.Text.Json.Utf8JsonReader reader, Type type, System.Text.Json.JsonSerializerOptions options) =>
            throw new NotSupportedException();
        public override void Write(System.Text.Json.Utf8JsonWriter writer, MessageCode value, System.Text.Json.JsonSerializerOptions options) =>
            writer.WriteStringValue(MessageCode.Wire(value));
    }

    /// <summary>The schema described as tools/check-contracts.py writes 0001_init.expected.txt.</summary>
    static string SchemaDump(SqliteConnection db)
    {
        List<object?[]> Rows(string sql)
        {
            using var c = db.CreateCommand();
            c.CommandText = sql;
            using var r = c.ExecuteReader();
            var rows = new List<object?[]>();
            while (r.Read())
            {
                var row = new object?[r.FieldCount];
                for (int i = 0; i < r.FieldCount; i++) row[i] = r.IsDBNull(i) ? null : r.GetValue(i);
                rows.Add(row);
            }
            return rows;
        }
        var lines = new List<string>();
        foreach (var t in Rows("SELECT name FROM sqlite_schema WHERE type = 'table' AND name NOT LIKE 'sqlite_%' ORDER BY name"))
        {
            var name = (string)t[0]!;
            lines.Add("table " + name);
            foreach (var c in Rows($"PRAGMA table_info({name})"))
                lines.Add($"  {c[1]} {c[2]}{(Convert.ToInt64(c[3]) != 0 ? " NOT NULL" : "")}{(c[4] is { } d ? " DEFAULT " + d : "")}{(Convert.ToInt64(c[5]) is var pk && pk != 0 ? " PK" + pk : "")}");
            foreach (var fk in Rows($"PRAGMA foreign_key_list({name})"))
                lines.Add($"  fk {fk[3]} -> {fk[2]}({fk[4]}) on delete {((string)fk[6]!).ToLowerInvariant()}");
        }
        foreach (var i in Rows("SELECT name, tbl_name, sql FROM sqlite_schema WHERE type = 'index' AND sql IS NOT NULL ORDER BY name"))
            lines.Add($"index {i[0]} on {i[1]}: {string.Join(" ", ((string)i[2]!).Split((char[]?)null, StringSplitOptions.RemoveEmptyEntries))}");
        lines.Add("user_version " + Rows("PRAGMA user_version")[0][0]);
        return string.Join("\n", lines) + "\n";
    }

    void Sql(string name, string sql)
    {
        using var db = new SqliteConnection($"Data Source={Path.Combine(_dir, name + ".sqlite")};Pooling=False");
        db.Open();
        using var c = db.CreateCommand();
        c.CommandText = sql;
        c.ExecuteNonQuery();
    }
}
