using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using Bromelia.Domain;
using Bromelia.Foundation;
using Bromelia.Ports;
using Microsoft.Data.Sqlite;

namespace Bromelia.Adapters;

/// <summary>The Store port on SQLite (plan §10.4, §23; shared/fixtures/adapters/store.cases.json). The shared
/// migrations run verbatim; connections use WAL, synchronous=FULL and foreign keys. One connection, every call
/// serialised (a read pool can come when measurements ask for it). Times are ISO 8601 text (Instant.Format), enums
/// their wire names, JSON columns the canonical encoding: BroError {code, params?, cause?}, BroMessage {code, params?,
/// severity? (when not info)}, JobRequest {kind, title, automatic, driveId?, mediaGeneration?, sessionId?, parentId?,
/// unitId?, options?, details?}, JobPlan {steps, mode?, profileId?, libraryId?, details?}, StepOutput {kind, data,
/// summary?}.</summary>
public sealed class SqliteStore : IStore, IDisposable
{
    readonly SqliteConnection _db;
    readonly IClock _clock;
    readonly SemaphoreSlim _lock = new(1, 1);
    readonly Db _top;

    SqliteStore(SqliteConnection db, IClock clock)
    {
        _db = db;
        _clock = clock;
        _top = new Db(this, inTransaction: false);
    }

    /// <summary>Opens (creating) the database file; ":memory:" for a private in-memory one (tests). Doesn't
    /// migrate.</summary>
    public static SqliteStore Open(string path, IClock clock)
    {
        try
        {
            var db = new SqliteConnection(new SqliteConnectionStringBuilder
            {
                DataSource = path,
                Mode = path == ":memory:" ? SqliteOpenMode.Memory : SqliteOpenMode.ReadWriteCreate,
                Pooling = false,
            }.ToString());
            db.Open();
            using (var c = db.CreateCommand())
            {
                c.CommandText = "PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL; PRAGMA foreign_keys=ON; PRAGMA busy_timeout=5000;";
                c.ExecuteNonQuery();
            }
            return new SqliteStore(db, clock);
        }
        catch (SqliteException e)
        {
            throw Failed("open", e);
        }
    }

    public IJobRepository Jobs() => new JobRepo(_top);
    public IStepRepository Steps() => new StepRepo(_top);
    public IUnitRepository Units() => new UnitRepo(_top);
    public ICatalogRepository Catalog() => new CatalogRepo(_top);
    public ICheckRepository Checks() => new CheckRepo(_top);
    public IReplicaRepository Replicas() => new ReplicaRepo(_top);
    public IDriveRepository Drives() => new DriveRepo(_top);
    public ILookupCacheRepository LookupCache() => new LookupRepo(_top);
    public IOutboxRepository Outbox() => new OutboxRepo(_top);
    public IKeyValueRepository Kv() => new KvRepo(_top);

    /// <summary>BEGIN IMMEDIATE … COMMIT around <paramref name="block"/>, which must use the store it is given (the
    /// store's lock is held meanwhile); a failure rolls back.</summary>
    public async Task<T> Transaction<T>(Func<IStore, Task<T>> block)
    {
        await _lock.WaitAsync().ConfigureAwait(false);
        try
        {
            Exec("BEGIN IMMEDIATE");
            try
            {
                var result = await block(new TxStore(new Db(this, inTransaction: true))).ConfigureAwait(false);
                Exec("COMMIT");
                return result;
            }
            catch
            {
                try { Exec("ROLLBACK"); } catch (SqliteException) { }
                throw;
            }
        }
        catch (SqliteException e)
        {
            throw Failed("transaction", e);
        }
        finally
        {
            _lock.Release();
        }
    }

    /// <summary>Applies the migrations whose number is above PRAGMA user_version, each in one transaction.</summary>
    public Task Migrate() => _top.Run(db =>
    {
        var current = Convert.ToInt32(Scalar(db, "PRAGMA user_version", null), CultureInfo.InvariantCulture);
        foreach (var (version, _, sql) in Migrations.All)
        {
            if (version <= current) continue;
            using var tx = db.BeginTransaction();
            using (var c = db.CreateCommand())
            {
                c.Transaction = tx;
                c.CommandText = sql;
                c.ExecuteNonQuery();
            }
            tx.Commit();
        }
        return 0;
    });

    public void Close()
    {
        _db.Close();
        _db.Dispose();
    }

    public void Dispose() => Close();

    void Exec(string sql)
    {
        using var c = _db.CreateCommand();
        c.CommandText = sql;
        c.ExecuteNonQuery();
    }

    static BroFailure Failed(string operation, Exception e) =>
        new(new BroMessage(MessageCode.StoreFailed, Severity.Error, ("operation", JsonValue.Of(operation)), ("reason", JsonValue.Of(e.Message))).ToError());

    // ---- execution ---------------------------------------------------------------------------------

    /// <summary>Runs one call on the connection: under the lock (on a worker thread) at the top level, directly inside
    /// a transaction, which already holds it.</summary>
    sealed class Db
    {
        public readonly SqliteStore Store;
        readonly bool _inTransaction;

        public Db(SqliteStore store, bool inTransaction)
        {
            Store = store;
            _inTransaction = inTransaction;
        }

        public Instant Now() => Store._clock.Now();

        public async Task<T> Run<T>(Func<SqliteConnection, T> body)
        {
            if (_inTransaction) return Guard(body);
            await Store._lock.WaitAsync().ConfigureAwait(false);
            try
            {
                return await Task.Run(() => Guard(body)).ConfigureAwait(false);
            }
            finally
            {
                Store._lock.Release();
            }
        }

        public Task Run(Action<SqliteConnection> body) => Run(db => { body(db); return 0; });

        /// <summary>Several statements that must happen together: their own transaction, unless already in one.</summary>
        public Task Atomic(Action<SqliteConnection> body) => Run(db =>
        {
            if (_inTransaction)
            {
                body(db);
                return 0;
            }
            using var tx = db.BeginTransaction();
            body(db);
            tx.Commit();
            return 0;
        });

        T Guard<T>(Func<SqliteConnection, T> body)
        {
            try { return body(Store._db); }
            catch (SqliteException e) { throw Failed("query", e); }
        }
    }

    sealed class TxStore : IStore
    {
        readonly Db _db;
        public TxStore(Db db) { _db = db; }
        public IJobRepository Jobs() => new JobRepo(_db);
        public IStepRepository Steps() => new StepRepo(_db);
        public IUnitRepository Units() => new UnitRepo(_db);
        public ICatalogRepository Catalog() => new CatalogRepo(_db);
        public ICheckRepository Checks() => new CheckRepo(_db);
        public IReplicaRepository Replicas() => new ReplicaRepo(_db);
        public IDriveRepository Drives() => new DriveRepo(_db);
        public ILookupCacheRepository LookupCache() => new LookupRepo(_db);
        public IOutboxRepository Outbox() => new OutboxRepo(_db);
        public IKeyValueRepository Kv() => new KvRepo(_db);
        public Task<T> Transaction<T>(Func<IStore, Task<T>> block) => block(this);
        public Task Migrate() => throw new InvalidOperationException("migrate outside a transaction");
    }

    // ---- SQL helpers -------------------------------------------------------------------------------

    static SqliteCommand Command(SqliteConnection db, string sql, params (string Name, object? Value)[] args)
    {
        var c = db.CreateCommand();
        c.CommandText = sql;
        foreach (var (name, value) in args) c.Parameters.AddWithValue(name, value ?? DBNull.Value);
        return c;
    }

    static void Execute(SqliteConnection db, string sql, params (string, object?)[] args)
    {
        using var c = Command(db, sql, args);
        c.ExecuteNonQuery();
    }

    static object? Scalar(SqliteConnection db, string sql, params (string, object?)[]? args)
    {
        using var c = Command(db, sql, args ?? Array.Empty<(string, object?)>());
        var v = c.ExecuteScalar();
        return v is DBNull ? null : v;
    }

    static List<T> Rows<T>(SqliteConnection db, string sql, Func<Row, T> map, params (string, object?)[] args)
    {
        using var c = Command(db, sql, args);
        using var r = c.ExecuteReader();
        var list = new List<T>();
        while (r.Read()) list.Add(map(new Row(r)));
        return list;
    }

    /// <summary>A result row, read by column name.</summary>
    sealed class Row
    {
        readonly SqliteDataReader _r;
        public Row(SqliteDataReader r) { _r = r; }
        bool IsNull(string c) => _r.IsDBNull(_r.GetOrdinal(c));
        public string Text(string c) => _r.GetString(_r.GetOrdinal(c));
        public string? TextOrNull(string c) => IsNull(c) ? null : Text(c);
        public long Long(string c) => _r.GetInt64(_r.GetOrdinal(c));
        public long? LongOrNull(string c) => IsNull(c) ? null : Long(c);
        public int Int(string c) => (int)Long(c);
        public int? IntOrNull(string c) => IsNull(c) ? null : Int(c);
        public double Double(string c) => _r.GetDouble(_r.GetOrdinal(c));
        public bool Bool(string c) => Long(c) != 0;
        public byte[] Blob(string c) => (byte[])_r.GetValue(_r.GetOrdinal(c));
        public Id IdOf(string c) => new(Text(c));
        public Id? IdOrNull(string c) => IsNull(c) ? null : new Id(Text(c));
        public Instant Time(string c) => Instant.Parse(Text(c)) ?? throw new FormatException(c);
        public Instant? TimeOrNull(string c) => IsNull(c) ? null : Time(c);
        public JsonValue? Json(string c) => IsNull(c) ? null : JsonValue.Parse(Text(c));
        public T Enum<T>(string c) where T : struct, System.Enum => EnumWire.Parse<T>(Text(c)) ?? throw new FormatException($"{c}: {Text(c)}");
        public T? EnumOrNull<T>(string c) where T : struct, System.Enum => IsNull(c) ? null : Enum<T>(c);
    }

    static string T(Instant i) => Instant.Format(i);
    static string? T(Instant? i) => i is { } v ? Instant.Format(v) : null;
    static string W<TE>(TE e) where TE : struct, System.Enum => EnumWire.Name(e);
    static string J(JsonValue v) => Encoding.UTF8.GetString(JsonValue.EncodeCanonical(v));
    static string? JOrNull(JsonValue? v) => v is null ? null : J(v);
    static long B(bool b) => b ? 1 : 0;

    // ---- JSON forms --------------------------------------------------------------------------------

    static JsonValue Obj(params (string Key, JsonValue? Value)[] members) =>
        new JsonValue.Object(members.Where(m => m.Value is not null).Select(m => new KeyValuePair<string, JsonValue>(m.Key, m.Value!)).ToList());

    static JsonValue ErrorJson(BroError e) =>
        Obj(("code", JsonValue.Of(e.Code)), ("params", e.Params.Members.Count > 0 ? e.Params : null), ("cause", e.Cause is { } c ? ErrorJson(c) : null));

    static BroError ErrorOf(JsonValue v) =>
        new(v["code"]?.AsString ?? "", v["params"] as JsonValue.Object ?? new JsonValue.Object(new List<KeyValuePair<string, JsonValue>>()),
            v["cause"] is { IsNull: false } c ? ErrorOf(c) : null);

    static JsonValue MessageJson(BroMessage m) =>
        Obj(("code", JsonValue.Of(MessageCode.Wire(m.Code))), ("params", m.Params.Members.Count > 0 ? m.Params : null),
            ("severity", m.Severity == Severity.Info ? null : JsonValue.Of(W(m.Severity))));

    static BroMessage MessageOf(JsonValue v) =>
        new(MessageCode.Parse(v["code"]?.AsString ?? "") ?? MessageCode.InternalUnexpected,
            v["params"] as JsonValue.Object ?? new JsonValue.Object(new List<KeyValuePair<string, JsonValue>>()),
            EnumWire.Parse<Severity>(v["severity"]?.AsString) ?? Severity.Info);

    static JsonValue RequestJson(JobRequest r) => Obj(
        ("kind", JsonValue.Of(W(r.Kind))), ("title", JsonValue.Of(r.Title)), ("automatic", JsonValue.Of(r.Automatic)),
        ("driveId", r.DriveId is { } d ? JsonValue.Of(d) : null), ("mediaGeneration", r.MediaGeneration is { } g ? JsonValue.Of(g) : null),
        ("sessionId", r.SessionId is { } s ? JsonValue.Of(s.Value) : null), ("parentId", r.ParentId is { } p ? JsonValue.Of(p.Value) : null),
        ("unitId", r.UnitId is { } u ? JsonValue.Of(u.Value) : null), ("options", r.Options), ("details", r.Details));

    static JobRequest RequestOf(JsonValue v) => new(
        EnumWire.Parse<JobKind>(v["kind"]?.AsString) ?? throw new FormatException("request.kind"), v["title"]?.AsString ?? "",
        v["automatic"]?.AsBool ?? false, v["driveId"]?.AsString, v["mediaGeneration"]?.AsInteger,
        v["sessionId"]?.AsString is { } s ? new Id(s) : null, v["parentId"]?.AsString is { } p ? new Id(p) : null,
        v["unitId"]?.AsString is { } u ? new Id(u) : null, v["options"], v["details"]);

    static JsonValue PlanJson(JobPlan p) => Obj(
        ("steps", JsonValue.Of(p.Steps.Select(s => JsonValue.Of(W(s))))), ("mode", p.Mode is { } m ? JsonValue.Of(m) : null),
        ("profileId", p.ProfileId is { } pr ? JsonValue.Of(pr) : null), ("libraryId", p.LibraryId is { } l ? JsonValue.Of(l) : null), ("details", p.Details));

    static JobPlan PlanOf(JsonValue v) => new(
        (v["steps"]?.AsArray ?? new List<JsonValue>()).Select(s => EnumWire.Parse<StepKind>(s.AsString) ?? throw new FormatException("plan.steps")).ToList(),
        v["mode"]?.AsString, v["profileId"]?.AsString, v["libraryId"]?.AsString, v["details"]);

    static JsonValue OutputJson(StepOutput o) =>
        Obj(("kind", JsonValue.Of(W(o.Kind))), ("data", o.Data), ("summary", o.Summary is { } s ? MessageJson(s) : null));

    static StepOutput OutputOf(JsonValue v) =>
        new(EnumWire.Parse<StepKind>(v["kind"]?.AsString) ?? throw new FormatException("output.kind"), v["data"] ?? JsonValue.Null.Instance,
            v["summary"] is { IsNull: false } s ? MessageOf(s) : null);

    static JsonValue Strings(IEnumerable<string> list) => JsonValue.Of(list.Select(JsonValue.Of));
    static List<string> StringsOf(JsonValue? v) => (v?.AsArray ?? new List<JsonValue>()).Select(x => x.AsString ?? "").ToList();

    // ---- jobs --------------------------------------------------------------------------------------

    sealed class JobRepo : IJobRepository
    {
        readonly Db _db;
        public JobRepo(Db db) { _db = db; }

        const string Columns = "id, kind, parent_id, state, outcome, error, blocked_by, decision, queue, position, drive_id, media_generation, automatic, mode, "
                               + "title, fingerprint, request, plan, unit_id, created_at, started_at, finished_at";

        static (string, object?)[] Args(JobRecord j) => new (string, object?)[]
        {
            ("$id", j.Id.Value), ("$kind", W(j.Kind)), ("$parent", j.ParentId?.Value), ("$state", W(j.State)), ("$outcome", j.Outcome is { } o ? W(o) : null),
            ("$error", j.Error is { } e ? J(ErrorJson(e)) : null), ("$blocked", JOrNull(j.BlockedBy)), ("$decision", JOrNull(j.Decision)), ("$queue", W(j.Queue)),
            ("$position", j.Position), ("$drive", j.DriveId), ("$generation", j.MediaGeneration), ("$automatic", B(j.Automatic)), ("$mode", j.Mode),
            ("$title", j.Title), ("$fingerprint", j.Fingerprint), ("$request", J(RequestJson(j.Request))), ("$plan", j.Plan is { } p ? J(PlanJson(p)) : null),
            ("$unit", j.UnitId?.Value), ("$created", T(j.CreatedAt)), ("$started", T(j.StartedAt)), ("$finished", T(j.FinishedAt)),
        };

        public static JobRecord Map(Row r) => new(
            r.IdOf("id"), r.Enum<JobKind>("kind"), r.IdOrNull("parent_id"), r.Enum<JobState>("state"), r.EnumOrNull<Outcome>("outcome"),
            r.Json("error") is { } e ? ErrorOf(e) : null, r.Json("blocked_by"), r.Json("decision"), r.Enum<Queue>("queue"), r.Int("position"),
            r.TextOrNull("drive_id"), r.LongOrNull("media_generation"), r.Bool("automatic"), r.TextOrNull("mode"), r.Text("title"), r.TextOrNull("fingerprint"),
            RequestOf(r.Json("request")!), r.Json("plan") is { } p ? PlanOf(p) : null, r.IdOrNull("unit_id"), r.Time("created_at"),
            r.TimeOrNull("started_at"), r.TimeOrNull("finished_at"));

        public Task Insert(JobRecord job) => _db.Run(db => Execute(db,
            $"INSERT INTO jobs ({Columns}) VALUES ($id, $kind, $parent, $state, $outcome, $error, $blocked, $decision, $queue, $position, $drive, "
            + "$generation, $automatic, $mode, $title, $fingerprint, $request, $plan, $unit, $created, $started, $finished)", Args(job)));

        public Task Update(JobRecord job) => _db.Run(db => Execute(db,
            "UPDATE jobs SET kind = $kind, parent_id = $parent, state = $state, outcome = $outcome, error = $error, blocked_by = $blocked, "
            + "decision = $decision, queue = $queue, position = $position, drive_id = $drive, media_generation = $generation, automatic = $automatic, "
            + "mode = $mode, title = $title, fingerprint = $fingerprint, request = $request, plan = $plan, unit_id = $unit, created_at = $created, "
            + "started_at = $started, finished_at = $finished WHERE id = $id", Args(job)));

        public Task<JobRecord?> Load(Id jobId) => _db.Run(db =>
            Rows(db, $"SELECT {Columns} FROM jobs WHERE id = $id", Map, ("$id", jobId.Value)).FirstOrDefault());

        public Task<IReadOnlyList<JobRecord>> Active() => _db.Run(db => (IReadOnlyList<JobRecord>)Rows(db,
            $"SELECT {Columns} FROM jobs WHERE state <> 'finished' ORDER BY "
            + "CASE queue WHEN 'acquisition' THEN 0 WHEN 'processing' THEN 1 ELSE 2 END, position, created_at, id", Map));

        public Task<IReadOnlyList<JobRecord>> Query(JobQuery q) => _db.Run(db => (IReadOnlyList<JobRecord>)Rows(db,
            $"SELECT {Columns} FROM jobs WHERE ($state IS NULL OR state = $state) AND ($kind IS NULL OR kind = $kind) "
            + "AND ($after IS NULL OR finished_at > $after) ORDER BY created_at DESC, id DESC LIMIT $limit OFFSET $offset", Map,
            ("$state", q.State is { } s ? W(s) : null), ("$kind", q.Kind is { } k ? W(k) : null), ("$after", T(q.FinishedAfter)),
            ("$limit", q.Limit), ("$offset", q.Offset)));
    }

    // ---- steps -------------------------------------------------------------------------------------

    sealed class StepRepo : IStepRepository
    {
        readonly Db _db;
        public StepRepo(Db db) { _db = db; }

        const string Columns = "job_id, seq, kind, state, attempt, started_at, finished_at, output, error, checkpoint";

        static StepRecord Map(Row r) => new(
            r.IdOf("job_id"), r.Int("seq"), r.Enum<StepKind>("kind"), r.Enum<StepState>("state"), r.Int("attempt"), r.TimeOrNull("started_at"),
            r.TimeOrNull("finished_at"), r.Json("output") is { } o ? OutputOf(o) : null, r.Json("error") is { } e ? ErrorOf(e) : null, r.Json("checkpoint"));

        public Task Save(StepRecord s) => _db.Run(db => Execute(db,
            $"INSERT OR REPLACE INTO job_steps ({Columns}) VALUES ($job, $seq, $kind, $state, $attempt, $started, $finished, $output, $error, $checkpoint)",
            ("$job", s.JobId.Value), ("$seq", s.Seq), ("$kind", W(s.Kind)), ("$state", W(s.State)), ("$attempt", s.Attempt), ("$started", T(s.StartedAt)),
            ("$finished", T(s.FinishedAt)), ("$output", s.Output is { } o ? J(OutputJson(o)) : null), ("$error", s.Error is { } e ? J(ErrorJson(e)) : null),
            ("$checkpoint", JOrNull(s.Checkpoint))));

        public Task<IReadOnlyList<StepRecord>> Completed(Id jobId) => _db.Run(db => (IReadOnlyList<StepRecord>)Rows(db,
            $"SELECT {Columns} FROM job_steps WHERE job_id = $job AND state = 'succeeded' ORDER BY seq", Map, ("$job", jobId.Value)));

        public Task<IReadOnlyList<StepRecord>> Load(Id jobId) => _db.Run(db => (IReadOnlyList<StepRecord>)Rows(db,
            $"SELECT {Columns} FROM job_steps WHERE job_id = $job ORDER BY seq", Map, ("$job", jobId.Value)));
    }

    // ---- units -------------------------------------------------------------------------------------

    const string UnitColumns = "u.id, u.library_id, u.physical_disc_id, u.job_id, u.path, u.record_file, u.record_version, u.state, u.status, u.name, u.kind, "
                               + "u.format, u.format_code, u.encrypted, u.fingerprint, u.label, u.season, u.part, u.volume, u.disc, u.makemkv_version, u.bytes, "
                               + "u.file_count, u.attempts, u.created_at, u.committed_at";

    static UnitRecord MapUnit(Row r) => new(
        r.IdOf("id"), r.Text("library_id"), r.IdOrNull("physical_disc_id"), r.IdOrNull("job_id"), r.Text("path"), r.Text("record_file"),
        r.Int("record_version"), r.Enum<UnitState>("state"), r.Enum<UnitStatus>("status"), r.Text("name"), r.Enum<MediaKind>("kind"), r.Text("format"),
        r.Text("format_code"), r.Bool("encrypted"), r.TextOrNull("fingerprint"), r.Text("label"), r.IntOrNull("season"), r.IntOrNull("part"),
        r.IntOrNull("volume"), r.IntOrNull("disc"), r.Text("makemkv_version"), r.Long("bytes"), r.Int("file_count"), r.Int("attempts"),
        r.Time("created_at"), r.TimeOrNull("committed_at"));

    static (string, object?)[] UnitArgs(UnitRecord u) => new (string, object?)[]
    {
        ("$id", u.Id.Value), ("$library", u.LibraryId), ("$disc_id", u.PhysicalDiscId?.Value), ("$job", u.JobId?.Value), ("$path", u.Path),
        ("$record", u.RecordFile), ("$version", u.RecordVersion), ("$state", W(u.State)), ("$status", W(u.Status)), ("$name", u.Name), ("$kind", W(u.Kind)),
        ("$format", u.Format), ("$code", u.FormatCode), ("$encrypted", B(u.Encrypted)), ("$fingerprint", u.Fingerprint), ("$label", u.Label),
        ("$season", u.Season), ("$part", u.Part), ("$volume", u.Volume), ("$disc", u.Disc), ("$makemkv", u.MakemkvVersion), ("$bytes", u.Bytes),
        ("$files", u.FileCount), ("$attempts", u.Attempts), ("$created", T(u.CreatedAt)), ("$committed", T(u.CommittedAt)),
    };

    const string UnitUpsert =
        "INSERT INTO archive_units (id, library_id, physical_disc_id, job_id, path, record_file, record_version, state, status, name, kind, format, "
        + "format_code, encrypted, fingerprint, label, season, part, volume, disc, makemkv_version, bytes, file_count, attempts, created_at, committed_at) "
        + "VALUES ($id, $library, $disc_id, $job, $path, $record, $version, $state, $status, $name, $kind, $format, $code, $encrypted, $fingerprint, "
        + "$label, $season, $part, $volume, $disc, $makemkv, $bytes, $files, $attempts, $created, $committed) "
        + "ON CONFLICT (id) DO UPDATE SET library_id = excluded.library_id, physical_disc_id = excluded.physical_disc_id, job_id = excluded.job_id, "
        + "path = excluded.path, record_file = excluded.record_file, record_version = excluded.record_version, state = excluded.state, "
        + "status = excluded.status, name = excluded.name, kind = excluded.kind, format = excluded.format, format_code = excluded.format_code, "
        + "encrypted = excluded.encrypted, fingerprint = excluded.fingerprint, label = excluded.label, season = excluded.season, part = excluded.part, "
        + "volume = excluded.volume, disc = excluded.disc, makemkv_version = excluded.makemkv_version, bytes = excluded.bytes, "
        + "file_count = excluded.file_count, attempts = excluded.attempts, created_at = excluded.created_at, committed_at = excluded.committed_at";

    sealed class UnitRepo : IUnitRepository
    {
        readonly Db _db;
        public UnitRepo(Db db) { _db = db; }

        public Task BeginCommit(UnitRecord unit, CommitIntent intent) => _db.Atomic(db =>
        {
            Execute(db, UnitUpsert, UnitArgs(unit));
            Execute(db, "INSERT INTO commit_intents (unit_id, job_id, staging, destination, merge, quarantine, created_at) "
                        + "VALUES ($unit, $job, $staging, $destination, $merge, $quarantine, $created)",
                ("$unit", intent.UnitId.Value), ("$job", intent.JobId.Value), ("$staging", intent.Staging), ("$destination", intent.Destination),
                ("$merge", B(intent.Merge)), ("$quarantine", B(intent.Quarantine)), ("$created", T(intent.CreatedAt)));
            foreach (var i in intent.Items)
                Execute(db, "INSERT INTO commit_items (unit_id, seq, from_path, to_path, sha256, is_dir, moved) VALUES ($unit, $seq, $from, $to, $sha, $dir, $moved)",
                    ("$unit", intent.UnitId.Value), ("$seq", i.Seq), ("$from", i.FromPath), ("$to", i.ToPath), ("$sha", i.Sha256), ("$dir", B(i.IsDir)),
                    ("$moved", B(i.Moved)));
        });

        public Task MarkMoved(Id unitId, int seq) => _db.Run(db =>
            Execute(db, "UPDATE commit_items SET moved = 1 WHERE unit_id = $unit AND seq = $seq", ("$unit", unitId.Value), ("$seq", seq)));

        public Task FinishCommit(CommitIntent intent, UnitRecord record, IReadOnlyList<UnitFile> files) => _db.Atomic(db =>
        {
            Execute(db, UnitUpsert, UnitArgs(record));
            Execute(db, "DELETE FROM unit_files WHERE unit_id = $unit", ("$unit", record.Id.Value));
            foreach (var f in files)
                Execute(db, "INSERT INTO unit_files (unit_id, path, size, sha256, role, title, episode) VALUES ($unit, $path, $size, $sha, $role, $title, $episode)",
                    ("$unit", record.Id.Value), ("$path", f.Path), ("$size", f.Size), ("$sha", f.Sha256), ("$role", f.Role), ("$title", f.Title),
                    ("$episode", f.Episode));
            Execute(db, "DELETE FROM commit_intents WHERE unit_id = $unit", ("$unit", intent.UnitId.Value));
        });

        public Task<IReadOnlyList<CommitIntent>> OpenIntents() => _db.Run(db =>
        {
            var intents = Rows(db, "SELECT unit_id, job_id, staging, destination, merge, quarantine, created_at FROM commit_intents ORDER BY created_at, unit_id",
                r => (Unit: r.IdOf("unit_id"), Job: r.IdOf("job_id"), Staging: r.Text("staging"), Destination: r.Text("destination"), Merge: r.Bool("merge"),
                    Quarantine: r.Bool("quarantine"), Created: r.Time("created_at")));
            return (IReadOnlyList<CommitIntent>)intents.Select(i => new CommitIntent(i.Unit, i.Job, i.Staging, i.Destination, i.Merge, i.Quarantine, i.Created,
                Rows(db, "SELECT seq, from_path, to_path, sha256, is_dir, moved FROM commit_items WHERE unit_id = $unit ORDER BY seq",
                    r => new CommitItem(r.Int("seq"), r.Text("from_path"), r.Text("to_path"), r.TextOrNull("sha256"), r.Bool("is_dir"), r.Bool("moved")),
                    ("$unit", i.Unit.Value)))).ToList();
        });

        public Task<UnitRecord?> Get(Id unitId) => _db.Run(db =>
            Rows(db, $"SELECT {UnitColumns} FROM archive_units u WHERE u.id = $id", MapUnit, ("$id", unitId.Value)).FirstOrDefault());

        public Task<IReadOnlyList<UnitRecord>> Query(UnitQuery q) => _db.Run(db => (IReadOnlyList<UnitRecord>)Rows(db,
            $"SELECT {UnitColumns} FROM archive_units u WHERE ($library IS NULL OR u.library_id = $library) "
            + "AND ($text IS NULL OR instr(lower(u.name), lower($text)) > 0) AND ($state IS NULL OR u.state = $state) "
            + "ORDER BY u.created_at DESC, u.id DESC LIMIT $limit OFFSET $offset", MapUnit,
            ("$library", q.LibraryId), ("$text", q.Text), ("$state", q.State is { } s ? W(s) : null), ("$limit", q.Limit), ("$offset", q.Offset)));

        public Task<IReadOnlyList<UnitRecord>> LeastRecentlyVerified(int limit) => _db.Run(db => (IReadOnlyList<UnitRecord>)Rows(db,
            $"SELECT {UnitColumns} FROM archive_units u WHERE u.state = 'committed' "
            + "ORDER BY (SELECT max(c.finished_at) FROM checks c WHERE c.unit_id = u.id) IS NOT NULL, "
            + "(SELECT max(c.finished_at) FROM checks c WHERE c.unit_id = u.id), u.created_at, u.id LIMIT $limit", MapUnit, ("$limit", limit)));

        public Task MarkMissing(Id unitId) => _db.Run(db =>
            Execute(db, "UPDATE archive_units SET state = 'missing' WHERE id = $id", ("$id", unitId.Value)));
    }

    // ---- catalogue ---------------------------------------------------------------------------------

    sealed class CatalogRepo : ICatalogRepository
    {
        readonly Db _db;
        public CatalogRepo(Db db) { _db = db; }

        public Task<IReadOnlyList<UnitRecord>> ArchivedBefore(string fingerprint) => _db.Run(db => (IReadOnlyList<UnitRecord>)Rows(db,
            $"SELECT {UnitColumns} FROM archive_units u WHERE u.fingerprint = $fp AND u.state = 'committed' ORDER BY u.committed_at DESC, u.id",
            MapUnit, ("$fp", fingerprint)));

        /// <summary>Committed TV discs (success or errors), as EpisodeContinuation sees them; only disc
        /// <paramref name="disc"/> when given.</summary>
        static List<ArchivedDisc> Candidates(SqliteConnection db, int? disc) => Rows(db,
            "SELECT u.name, u.label, u.season, u.part, u.volume, u.disc, l.path AS library_path, u.path, "
            + "(SELECT max(f.episode) FROM unit_files f WHERE f.unit_id = u.id) AS last_episode "
            + "FROM archive_units u JOIN libraries l ON l.id = u.library_id WHERE u.kind = 'tv' AND u.state = 'committed' "
            + "AND u.status IN ('success', 'errors') AND ($disc IS NULL OR u.disc = $disc) ORDER BY u.committed_at, u.id",
            r => new ArchivedDisc(r.Text("name"), LabelParser.Parse(r.Text("label")).Title, r.IntOrNull("season"), r.IntOrNull("part"), r.IntOrNull("volume"),
                r.IntOrNull("disc"), r.IntOrNull("last_episode"), r.Text("library_path").TrimEnd('/', '\\') + "/" + r.Text("path")),
            ("$disc", disc));

        public Task<ArchivedDisc?> PreviousDisc(ContinuationQuery query) => _db.Run(db =>
            query.Disc <= 1 ? null
            : Candidates(db, query.Disc - 1).Where(c => EpisodeContinuation.SameSet(c, query) && c.LastEpisode != null)
                .OrderByDescending(c => c.LastEpisode).FirstOrDefault());

        public Task<int?> HighestEpisode(ContinuationQuery query) => _db.Run(db =>
            Candidates(db, null).Where(c => EpisodeContinuation.SameSet(c, query)).Max(c => c.LastEpisode));

        public Task<IReadOnlyList<WorkRecord>> Works(string query) => _db.Run(db => (IReadOnlyList<WorkRecord>)Rows(db,
            "SELECT id, kind, title, year, tmdb_id, imdb_id, created_at, updated_at FROM works WHERE instr(lower(title), lower($q)) > 0 "
            + "ORDER BY title COLLATE NOCASE, id",
            r => new WorkRecord(r.IdOf("id"), r.Enum<MediaKind>("kind"), r.Text("title"), r.IntOrNull("year"), r.IntOrNull("tmdb_id"), r.TextOrNull("imdb_id"),
                r.Time("created_at"), r.Time("updated_at")), ("$q", query)));

        public Task<DiscSetRecord?> Set(Id setId) => _db.Run(db => Rows(db,
            "SELECT id, work_id, label_title, season, part, volume, description, known_count FROM disc_sets WHERE id = $id",
            r => new DiscSetRecord(r.IdOf("id"), r.IdOrNull("work_id"), r.Text("label_title"), r.IntOrNull("season"), r.IntOrNull("part"),
                r.IntOrNull("volume"), r.Text("description"), r.IntOrNull("known_count")), ("$id", setId.Value)).FirstOrDefault());
    }

    // ---- checks, replicas, drives -------------------------------------------------------------------

    sealed class CheckRepo : ICheckRepository
    {
        readonly Db _db;
        public CheckRepo(Db db) { _db = db; }

        const string Columns = "id, unit_id, replica_id, folder, job_id, started_at, finished_at, result, files, bytes, changed, unreadable, missing, unlisted, error";

        static CheckRecord Map(Row r) => new(
            r.IdOf("id"), r.IdOrNull("unit_id"), r.IdOrNull("replica_id"), r.Text("folder"), r.IdOrNull("job_id"), r.Time("started_at"),
            r.TimeOrNull("finished_at"), r.Enum<CheckResult>("result"), r.Int("files"), r.Long("bytes"), StringsOf(r.Json("changed")),
            StringsOf(r.Json("unreadable")), StringsOf(r.Json("missing")), StringsOf(r.Json("unlisted")), r.Json("error") is { } e ? MessageOf(e) : null);

        public Task Insert(CheckRecord c) => _db.Run(db => Execute(db,
            $"INSERT INTO checks ({Columns}) VALUES ($id, $unit, $replica, $folder, $job, $started, $finished, $result, $files, $bytes, $changed, "
            + "$unreadable, $missing, $unlisted, $error)",
            ("$id", c.Id.Value), ("$unit", c.UnitId?.Value), ("$replica", c.ReplicaId?.Value), ("$folder", c.Folder), ("$job", c.JobId?.Value),
            ("$started", T(c.StartedAt)), ("$finished", T(c.FinishedAt)), ("$result", W(c.Result)), ("$files", c.Files), ("$bytes", c.Bytes),
            ("$changed", J(Strings(c.Changed))), ("$unreadable", J(Strings(c.Unreadable))), ("$missing", J(Strings(c.Missing))),
            ("$unlisted", J(Strings(c.Unlisted))), ("$error", c.Error is { } e ? J(MessageJson(e)) : null)));

        public Task<IReadOnlyList<CheckRecord>> ForUnit(Id unitId) => _db.Run(db => (IReadOnlyList<CheckRecord>)Rows(db,
            $"SELECT {Columns} FROM checks WHERE unit_id = $unit ORDER BY started_at DESC, id DESC", Map, ("$unit", unitId.Value)));

        public Task<CheckRecord?> Latest(string folder) => _db.Run(db => Rows(db,
            $"SELECT {Columns} FROM checks WHERE folder = $folder ORDER BY coalesce(finished_at, started_at) DESC, id DESC LIMIT 1", Map,
            ("$folder", folder)).FirstOrDefault());
    }

    sealed class ReplicaRepo : IReplicaRepository
    {
        readonly Db _db;
        public ReplicaRepo(Db db) { _db = db; }

        static ReplicaRecord Map(Row r) => new(r.IdOf("id"), r.IdOf("unit_id"), r.Text("target_id"), r.Text("path"), r.Enum<ReplicaState>("state"),
            r.TimeOrNull("verified_at"), r.Json("error") is { } e ? MessageOf(e) : null);

        public Task Upsert(ReplicaRecord x) => _db.Run(db => Execute(db,
            "INSERT INTO replicas (id, unit_id, target_id, path, state, verified_at, error) VALUES ($id, $unit, $target, $path, $state, $verified, $error) "
            + "ON CONFLICT (id) DO UPDATE SET unit_id = excluded.unit_id, target_id = excluded.target_id, path = excluded.path, state = excluded.state, "
            + "verified_at = excluded.verified_at, error = excluded.error",
            ("$id", x.Id.Value), ("$unit", x.UnitId.Value), ("$target", x.TargetId), ("$path", x.Path), ("$state", W(x.State)),
            ("$verified", T(x.VerifiedAt)), ("$error", x.Error is { } e ? J(MessageJson(e)) : null)));

        public Task<IReadOnlyList<ReplicaRecord>> ForUnit(Id unitId) => _db.Run(db => (IReadOnlyList<ReplicaRecord>)Rows(db,
            "SELECT id, unit_id, target_id, path, state, verified_at, error FROM replicas WHERE unit_id = $unit ORDER BY target_id, id", Map,
            ("$unit", unitId.Value)));

        public Task<IReadOnlyList<ReplicaRecord>> Lagging() => _db.Run(db => (IReadOnlyList<ReplicaRecord>)Rows(db,
            "SELECT id, unit_id, target_id, path, state, verified_at, error FROM replicas WHERE state <> 'verified' ORDER BY id", Map));
    }

    sealed class DriveRepo : IDriveRepository
    {
        readonly Db _db;
        public DriveRepo(Db db) { _db = db; }

        public Task Upsert(DriveRecord d) => _db.Run(db => Execute(db,
            "INSERT INTO drives (id, identification, model, last_device, config_id, first_seen_at, last_seen_at) "
            + "VALUES ($id, $ident, $model, $device, $config, $first, $last) ON CONFLICT (id) DO UPDATE SET identification = excluded.identification, "
            + "model = excluded.model, last_device = excluded.last_device, config_id = excluded.config_id, last_seen_at = excluded.last_seen_at",
            ("$id", d.Id), ("$ident", d.Identification), ("$model", d.Model), ("$device", d.LastDevice), ("$config", d.ConfigId),
            ("$first", T(d.FirstSeenAt)), ("$last", T(d.LastSeenAt))));

        public Task<IReadOnlyList<DriveRecord>> All() => _db.Run(db => (IReadOnlyList<DriveRecord>)Rows(db,
            "SELECT id, identification, model, last_device, config_id, first_seen_at, last_seen_at FROM drives ORDER BY id",
            r => new DriveRecord(r.Text("id"), r.Text("identification"), r.Text("model"), r.Text("last_device"), r.TextOrNull("config_id"),
                r.Time("first_seen_at"), r.Time("last_seen_at"))));

        public Task RecordStats(string driveId, string day, DriveStats s) => _db.Run(db => Execute(db,
            "INSERT INTO drive_stats_daily (drive_id, day, jobs, failed_jobs, read_error_jobs, read_errors, bytes_read, seconds_reading) "
            + "VALUES ($drive, $day, $jobs, $failed, $rejobs, $errors, $bytes, $seconds) ON CONFLICT (drive_id, day) DO UPDATE SET "
            + "jobs = jobs + excluded.jobs, failed_jobs = failed_jobs + excluded.failed_jobs, read_error_jobs = read_error_jobs + excluded.read_error_jobs, "
            + "read_errors = read_errors + excluded.read_errors, bytes_read = bytes_read + excluded.bytes_read, "
            + "seconds_reading = seconds_reading + excluded.seconds_reading",
            ("$drive", driveId), ("$day", day), ("$jobs", s.Jobs), ("$failed", s.FailedJobs), ("$rejobs", s.ReadErrorJobs), ("$errors", s.ReadErrors),
            ("$bytes", s.BytesRead), ("$seconds", s.SecondsReading)));
    }

    // ---- services ----------------------------------------------------------------------------------

    sealed class LookupRepo : ILookupCacheRepository
    {
        readonly Db _db;
        public LookupRepo(Db db) { _db = db; }

        public Task<HttpResponse?> Get(string provider, string key) => _db.Run(db => Rows(db,
            "SELECT status, response FROM lookup_cache WHERE provider = $p AND request_key = $k AND expires_at > $now",
            r => new HttpResponse(r.Int("status"), new List<KeyValuePair<string, string>>(), r.Blob("response")),
            ("$p", provider), ("$k", key), ("$now", T(_db.Now()))).FirstOrDefault());

        public Task Put(string provider, string key, HttpResponse response, Instant expiresAt) => _db.Run(db => Execute(db,
            "INSERT OR REPLACE INTO lookup_cache (provider, request_key, status, response, fetched_at, expires_at) VALUES ($p, $k, $status, $body, $now, $expires)",
            ("$p", provider), ("$k", key), ("$status", response.Status), ("$body", response.Body), ("$now", T(_db.Now())), ("$expires", T(expiresAt))));
    }

    sealed class OutboxRepo : IOutboxRepository
    {
        readonly Db _db;
        public OutboxRepo(Db db) { _db = db; }

        const string Columns = "id, target_id, job_id, title, body, status, created_at, attempts, next_attempt_at, sent_at, last_error";

        static OutboxEntry Map(Row r) => new(
            r.IdOf("id"), r.Text("target_id"), r.IdOrNull("job_id"), MessageOf(r.Json("title")!),
            (r.Json("body")?.AsArray ?? new List<JsonValue>()).Select(MessageOf).ToList(), r.Enum<StatusWord>("status"), r.Time("created_at"),
            r.Int("attempts"), r.TimeOrNull("next_attempt_at"), r.TimeOrNull("sent_at"), r.Json("last_error") is { } e ? ErrorOf(e) : null);

        public Task Add(OutboxEntry n) => _db.Run(db => Execute(db,
            $"INSERT INTO outbox ({Columns}) VALUES ($id, $target, $job, $title, $body, $status, $created, $attempts, $next, $sent, $error)",
            ("$id", n.Id.Value), ("$target", n.TargetId), ("$job", n.JobId?.Value), ("$title", J(MessageJson(n.Title))),
            ("$body", J(JsonValue.Of(n.Lines.Select(MessageJson)))), ("$status", W(n.Status)), ("$created", T(n.CreatedAt)), ("$attempts", n.Attempts),
            ("$next", T(n.NextAttemptAt)), ("$sent", T(n.SentAt)), ("$error", n.LastError is { } e ? J(ErrorJson(e)) : null)));

        public Task<IReadOnlyList<OutboxEntry>> Due(Instant now) => _db.Run(db => (IReadOnlyList<OutboxEntry>)Rows(db,
            $"SELECT {Columns} FROM outbox WHERE sent_at IS NULL AND next_attempt_at IS NOT NULL AND next_attempt_at <= $now "
            + "ORDER BY next_attempt_at, created_at, id", Map, ("$now", T(now))));

        public Task MarkSent(Id id) => _db.Run(db => Execute(db,
            "UPDATE outbox SET sent_at = $now, attempts = attempts + 1, last_error = NULL WHERE id = $id", ("$id", id.Value), ("$now", T(_db.Now()))));

        public Task MarkFailed(Id id, BroError error, Instant? retryAt) => _db.Run(db => Execute(db,
            "UPDATE outbox SET attempts = attempts + 1, last_error = $error, next_attempt_at = $retry WHERE id = $id",
            ("$id", id.Value), ("$error", J(ErrorJson(error))), ("$retry", T(retryAt))));
    }

    sealed class KvRepo : IKeyValueRepository
    {
        readonly Db _db;
        public KvRepo(Db db) { _db = db; }

        public Task<JsonValue?> Get(string key) => _db.Run(db =>
            Scalar(db, "SELECT value FROM kv WHERE key = $k", ("$k", key)) is string text ? JsonValue.Parse(text) : null);

        public Task Set(string key, JsonValue value) => _db.Run(db => Execute(db,
            "INSERT INTO kv (key, value, updated_at) VALUES ($k, $v, $now) ON CONFLICT (key) DO UPDATE SET value = excluded.value, updated_at = excluded.updated_at",
            ("$k", key), ("$v", J(value)), ("$now", T(_db.Now()))));
    }
}
