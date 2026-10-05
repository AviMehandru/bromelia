import BroDomain
import BroFoundation
import BroPorts
import Foundation
import SQLite3

/// The Store port on SQLite (plan §10.4, §23; shared/fixtures/adapters/store.cases.json), on the system SQLite3. The
/// shared migrations run verbatim; the connection uses WAL, synchronous=FULL and foreign keys. One connection, every
/// call serialised (a read pool can come when measurements ask for it). Times are ISO 8601 text (Instant.format),
/// enums their wire names, JSON columns the canonical encoding: BroError {code, params?, cause?}, BroMessage {code,
/// params?, severity? (when not info)}, JobRequest {kind, title, automatic, driveId?, mediaGeneration?, sessionId?,
/// parentId?, unitId?, options?, details?}, JobPlan {steps, mode?, profileId?, libraryId?, details?}, StepOutput {kind,
/// data, summary?}.
public final class SqliteStore: Store, @unchecked Sendable {
    private let db: OpaquePointer
    private let clock: any Clock
    private let lock = AsyncLock()
    private var closed = false
    /// The store whose transaction this task is inside: its outer repositories would wait for ever for the lock the
    /// transaction holds.
    @TaskLocal fileprivate static var inTransactionOf: ObjectIdentifier?

    private init(db: OpaquePointer, clock: any Clock) {
        self.db = db
        self.clock = clock
    }

    /// Opens (creating) the database file; ":memory:" for a private in-memory one (tests). Doesn't migrate.
    public static func open(_ path: String, clock: any Clock) throws(BroError) -> SqliteStore {
        var handle: OpaquePointer?
        let rc = sqlite3_open_v2(path, &handle, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nil)
        guard rc == SQLITE_OK, let handle else {
            let reason = handle.map { String(cString: sqlite3_errmsg($0)) } ?? String(cString: sqlite3_errstr(rc))
            if let handle { sqlite3_close(handle) }
            throw failed("open", reason)
        }
        let store = SqliteStore(db: handle, clock: clock)
        let mode = try store.rows("PRAGMA journal_mode=WAL", []) { $0.text(0) }.first ?? ""
        // SQLite answers with the mode it is in: anything but WAL (a network share, a read-only folder) isn't safe here.
        guard mode == "wal" || (path == ":memory:" && mode == "memory") else {
            throw failed("open", "the journal is \(mode), not WAL")
        }
        try store.exec("PRAGMA synchronous=FULL; PRAGMA foreign_keys=ON; PRAGMA busy_timeout=5000;")
        return store
    }

    deinit { close() }

    public func jobs() -> any JobRepository { JobRepo(Db(self, inTransaction: false)) }
    public func steps() -> any StepRepository { StepRepo(Db(self, inTransaction: false)) }
    public func units() -> any UnitRepository { UnitRepo(Db(self, inTransaction: false)) }
    public func catalog() -> any CatalogRepository { CatalogRepo(Db(self, inTransaction: false)) }
    public func checks() -> any CheckRepository { CheckRepo(Db(self, inTransaction: false)) }
    public func replicas() -> any ReplicaRepository { ReplicaRepo(Db(self, inTransaction: false)) }
    public func drives() -> any DriveRepository { DriveRepo(Db(self, inTransaction: false)) }
    public func lookupCache() -> any LookupCacheRepository { LookupRepo(Db(self, inTransaction: false)) }
    public func outbox() -> any OutboxRepository { OutboxRepo(Db(self, inTransaction: false)) }
    public func kv() -> any KeyValueRepository { KvRepo(Db(self, inTransaction: false)) }

    /// BEGIN IMMEDIATE … COMMIT around block, which must use the store it is given (the store's lock is held
    /// meanwhile: the outer store fails with store.reentered); a failure rolls back.
    public func transaction<T: Sendable>(_ block: @Sendable (any Store) async throws(BroError) -> T) async throws(BroError) -> T {
        try checkNotReentered()
        await lock.acquire()
        defer { lock.release() }
        try exec("BEGIN IMMEDIATE")
        let result: Result<T, BroError> = await Self.$inTransactionOf.withValue(ObjectIdentifier(self)) {
            do throws(BroError) { return .success(try await block(TxStore(Db(self, inTransaction: true)))) } catch { return .failure(error) }
        }
        do throws(BroError) {
            let value = try result.get()
            try exec("COMMIT")
            return value
        } catch {
            try? exec("ROLLBACK")
            throw error
        }
    }

    /// store.reentered when this task is inside one of this store's transactions.
    fileprivate func checkNotReentered() throws(BroError) {
        if Self.inTransactionOf == ObjectIdentifier(self) { throw BroMessage(.storeReentered, severity: .error).toError() }
    }

    /// Applies the migrations whose number is above PRAGMA user_version, each in one transaction. A database newer than
    /// every migration (a newer Bromelia's) is refused: store.tooNew.
    public func migrate() async throws(BroError) {
        try await Db(self, inTransaction: false).run { s throws(BroError) in
            let current = try s.rows("PRAGMA user_version", []) { $0.int(0) }.first ?? 0
            let newest = Migrations.all.map(\.version).max() ?? 0
            if current > newest {
                throw BroMessage(.storeTooNew, [("version", .integer(Int64(current))), ("newest", .integer(Int64(newest)))], severity: .error).toError()
            }
            for m in Migrations.all where m.version > current {
                try s.exec("BEGIN")
                do throws(BroError) {
                    try s.exec(m.sql)
                    try s.exec("COMMIT")
                } catch {
                    try? s.exec("ROLLBACK")
                    throw error
                }
            }
        }
    }

    public func close() {
        if closed { return }
        closed = true
        sqlite3_close_v2(db)
    }

    static func failed(_ operation: String, _ reason: String) -> BroError {
        BroMessage(.storeFailed, [("operation", .string(operation)), ("reason", .string(reason))], severity: .error).toError()
    }

    // ---- SQL ----------------------------------------------------------------------------------------

    fileprivate func exec(_ sql: String) throws(BroError) {
        var message: UnsafeMutablePointer<CChar>?
        if sqlite3_exec(db, sql, nil, nil, &message) != SQLITE_OK {
            let reason = message.map { String(cString: $0) } ?? String(cString: sqlite3_errmsg(db))
            sqlite3_free(message)
            throw Self.failed("query", reason)
        }
    }

    fileprivate func execute(_ sql: String, _ args: [Value]) throws(BroError) {
        let stmt = try prepare(sql, args)
        defer { sqlite3_finalize(stmt) }
        let rc = sqlite3_step(stmt)
        guard rc == SQLITE_DONE || rc == SQLITE_ROW else { throw Self.failed("query", String(cString: sqlite3_errmsg(db))) }
    }

    fileprivate func rows<T>(_ sql: String, _ args: [Value], _ map: (Row) throws(BroError) -> T) throws(BroError) -> [T] {
        let stmt = try prepare(sql, args)
        defer { sqlite3_finalize(stmt) }
        var out: [T] = []
        while true {
            let rc = sqlite3_step(stmt)
            if rc == SQLITE_DONE { return out }
            guard rc == SQLITE_ROW else { throw Self.failed("query", String(cString: sqlite3_errmsg(db))) }
            out.append(try map(Row(stmt: stmt)))
        }
    }

    private func prepare(_ sql: String, _ args: [Value]) throws(BroError) -> OpaquePointer {
        var stmt: OpaquePointer?
        guard sqlite3_prepare_v2(db, sql, -1, &stmt, nil) == SQLITE_OK, let stmt else {
            throw Self.failed("query", String(cString: sqlite3_errmsg(db)))
        }
        let transient = unsafeBitCast(-1, to: sqlite3_destructor_type.self)
        for (i, v) in args.enumerated() {
            let index = Int32(i + 1)
            switch v {
            case .null: sqlite3_bind_null(stmt, index)
            case .int(let n): sqlite3_bind_int64(stmt, index, n)
            case .double(let d): sqlite3_bind_double(stmt, index, d)
            case .text(let s): sqlite3_bind_text(stmt, index, s, -1, transient)
            case .blob(let b): b.withUnsafeBytes { _ = sqlite3_bind_blob(stmt, index, $0.baseAddress, Int32(b.count), transient) }
            }
        }
        return stmt
    }

    // ---- execution ----------------------------------------------------------------------------------

    /// Runs one call on the connection: under the lock at the top level, directly inside a transaction, which holds
    /// it already.
    fileprivate struct Db: Sendable {
        let store: SqliteStore
        let inTransaction: Bool

        init(_ store: SqliteStore, inTransaction: Bool) {
            self.store = store
            self.inTransaction = inTransaction
        }

        var now: Instant { store.clock.now() }

        func run<T>(_ body: (SqliteStore) throws(BroError) -> T) async throws(BroError) -> T {
            if inTransaction { return try body(store) }
            try store.checkNotReentered()
            await store.lock.acquire()
            defer { store.lock.release() }
            return try body(store)
        }

        /// Several statements that must happen together: their own transaction, unless already in one.
        func atomic(_ body: (SqliteStore) throws(BroError) -> Void) async throws(BroError) {
            let nested = inTransaction
            try await run { s throws(BroError) in
                if nested { return try body(s) }
                try s.exec("BEGIN IMMEDIATE")
                do throws(BroError) {
                    try body(s)
                    try s.exec("COMMIT")
                } catch {
                    try? s.exec("ROLLBACK")
                    throw error
                }
            }
        }
    }

    private struct TxStore: Store {
        let db: Db
        init(_ db: Db) { self.db = db }
        func jobs() -> any JobRepository { JobRepo(db) }
        func steps() -> any StepRepository { StepRepo(db) }
        func units() -> any UnitRepository { UnitRepo(db) }
        func catalog() -> any CatalogRepository { CatalogRepo(db) }
        func checks() -> any CheckRepository { CheckRepo(db) }
        func replicas() -> any ReplicaRepository { ReplicaRepo(db) }
        func drives() -> any DriveRepository { DriveRepo(db) }
        func lookupCache() -> any LookupCacheRepository { LookupRepo(db) }
        func outbox() -> any OutboxRepository { OutboxRepo(db) }
        func kv() -> any KeyValueRepository { KvRepo(db) }
        func transaction<T: Sendable>(_ block: @Sendable (any Store) async throws(BroError) -> T) async throws(BroError) -> T {
            try await block(self)
        }
        func migrate() async throws(BroError) { throw SqliteStore.failed("migrate", "inside a transaction") }
    }
}

// ---- values and rows ------------------------------------------------------------------------------

fileprivate enum Value {
    case null
    case int(Int64)
    case double(Double)
    case text(String)
    case blob([UInt8])

    static func of(_ s: String?) -> Value { s.map { .text($0) } ?? .null }
    static func of(_ n: Int?) -> Value { n.map { .int(Int64($0)) } ?? .null }
    static func of(_ n: Int64?) -> Value { n.map { .int($0) } ?? .null }
    static func of(_ b: Bool) -> Value { .int(b ? 1 : 0) }
    static func of(_ i: Instant?) -> Value { i.map { .text(Instant.format($0)) } ?? .null }
    static func of(_ id: Id?) -> Value { id.map { .text($0.value) } ?? .null }
    static func json(_ v: JsonValue?) -> Value { v.map { .text(String(decoding: JsonValue.encodeCanonical($0), as: UTF8.self)) } ?? .null }
}

/// A result row, read by column index (the SELECTs list their columns).
fileprivate struct Row {
    let stmt: OpaquePointer

    func isNull(_ i: Int32) -> Bool { sqlite3_column_type(stmt, i) == SQLITE_NULL }
    func text(_ i: Int32) -> String { sqlite3_column_text(stmt, i).map { String(cString: $0) } ?? "" }
    func textOrNil(_ i: Int32) -> String? { isNull(i) ? nil : text(i) }
    func int64(_ i: Int32) -> Int64 { sqlite3_column_int64(stmt, i) }
    func int(_ i: Int32) -> Int { Int(int64(i)) }
    func intOrNil(_ i: Int32) -> Int? { isNull(i) ? nil : int(i) }
    func int64OrNil(_ i: Int32) -> Int64? { isNull(i) ? nil : int64(i) }
    func double(_ i: Int32) -> Double { sqlite3_column_double(stmt, i) }
    func bool(_ i: Int32) -> Bool { int64(i) != 0 }
    func blob(_ i: Int32) -> [UInt8] {
        let n = Int(sqlite3_column_bytes(stmt, i))
        guard let p = sqlite3_column_blob(stmt, i), n > 0 else { return [] }
        return Array(UnsafeRawBufferPointer(start: p, count: n))
    }
    func id(_ i: Int32) -> Id { Id(text(i)) }
    func idOrNil(_ i: Int32) -> Id? { isNull(i) ? nil : id(i) }
    func time(_ i: Int32) throws(BroError) -> Instant {
        guard let t = Instant.parse(text(i)) else { throw SqliteStore.failed("read", "not a time: \(text(i))") }
        return t
    }
    func timeOrNil(_ i: Int32) throws(BroError) -> Instant? { isNull(i) ? nil : try time(i) }
    func json(_ i: Int32) -> JsonValue? { isNull(i) ? nil : JsonValue.parse(text(i)) }
    func wire<E: RawRepresentable>(_ i: Int32) throws(BroError) -> E where E.RawValue == String {
        guard let e = E(rawValue: text(i)) else { throw SqliteStore.failed("read", "unknown value \(text(i))") }
        return e
    }
    func wireOrNil<E: RawRepresentable>(_ i: Int32) throws(BroError) -> E? where E.RawValue == String { isNull(i) ? nil : try wire(i) }
}

// ---- JSON forms -----------------------------------------------------------------------------------

private func object(_ members: [(String, JsonValue?)]) -> JsonValue {
    .object(members.compactMap { m in m.1.map { (key: m.0, value: $0) } })
}

private func errorJson(_ e: BroError) -> JsonValue {
    object([("code", .string(e.code)), ("params", e.params.isEmpty ? nil : .object(e.params)), ("cause", e.cause.map(errorJson))])
}

private func errorOf(_ v: JsonValue) -> BroError {
    BroError(v["code"]?.string ?? "", v["params"]?.members ?? [], cause: v["cause"].flatMap { $0.isNull ? nil : errorOf($0) })
}

private func messageJson(_ m: BroMessage) -> JsonValue {
    object([("code", .string(m.code.rawValue)), ("params", m.params.isEmpty ? nil : .object(m.params)),
            ("severity", m.severity == .info ? nil : .string(m.severity.rawValue))])
}

private func messageOf(_ v: JsonValue) -> BroMessage {
    BroMessage(MessageCode(rawValue: v["code"]?.string ?? "") ?? .internalUnexpected, v["params"]?.members ?? [],
               severity: v["severity"]?.string.flatMap(Severity.init(rawValue:)) ?? .info)
}

private func requestJson(_ r: JobRequest) -> JsonValue {
    object([("kind", .string(r.kind.rawValue)), ("title", .string(r.title)), ("automatic", .bool(r.automatic)),
            ("driveId", r.driveId.map { .string($0) }), ("mediaGeneration", r.mediaGeneration.map { .integer($0) }),
            ("sessionId", r.sessionId.map { .string($0.value) }), ("parentId", r.parentId.map { .string($0.value) }),
            ("unitId", r.unitId.map { .string($0.value) }), ("options", r.options), ("details", r.details)])
}

private func requestOf(_ v: JsonValue) throws(BroError) -> JobRequest {
    guard let kind = v["kind"]?.string.flatMap(JobKind.init(rawValue:)) else { throw SqliteStore.failed("read", "request.kind") }
    return JobRequest(kind: kind, title: v["title"]?.string ?? "", automatic: v["automatic"]?.bool ?? false, driveId: v["driveId"]?.string,
                      mediaGeneration: v["mediaGeneration"]?.int, sessionId: v["sessionId"]?.string.map(Id.init),
                      parentId: v["parentId"]?.string.map(Id.init), unitId: v["unitId"]?.string.map(Id.init), options: v["options"],
                      details: v["details"])
}

private func planJson(_ p: JobPlan) -> JsonValue {
    object([("steps", .array(p.steps.map { .string($0.rawValue) })), ("mode", p.mode.map { .string($0) }),
            ("profileId", p.profileId.map { .string($0) }), ("libraryId", p.libraryId.map { .string($0) }), ("details", p.details)])
}

private func planOf(_ v: JsonValue) throws(BroError) -> JobPlan {
    var steps: [StepKind] = []
    for s in v["steps"]?.array ?? [] {
        guard let k = s.string.flatMap(StepKind.init(rawValue:)) else { throw SqliteStore.failed("read", "plan.steps") }
        steps.append(k)
    }
    return JobPlan(steps: steps, mode: v["mode"]?.string, profileId: v["profileId"]?.string, libraryId: v["libraryId"]?.string, details: v["details"])
}

private func outputJson(_ o: StepOutput) -> JsonValue {
    object([("kind", .string(o.kind.rawValue)), ("data", o.data), ("summary", o.summary.map(messageJson))])
}

private func outputOf(_ v: JsonValue) throws(BroError) -> StepOutput {
    guard let kind = v["kind"]?.string.flatMap(StepKind.init(rawValue:)) else { throw SqliteStore.failed("read", "output.kind") }
    return StepOutput(kind: kind, data: v["data"] ?? .null, summary: v["summary"].flatMap { $0.isNull ? nil : messageOf($0) })
}

private func strings(_ list: [String]) -> JsonValue { .array(list.map { .string($0) }) }
private func stringsOf(_ v: JsonValue?) -> [String] { (v?.array ?? []).map { $0.string ?? "" } }

// ---- jobs -----------------------------------------------------------------------------------------

private let jobColumns = "id, kind, parent_id, state, outcome, error, blocked_by, decision, queue, position, drive_id, media_generation, automatic, mode, "
    + "title, fingerprint, request, plan, unit_id, created_at, started_at, finished_at"

private func jobArgs(_ j: JobRecord) -> [Value] {
    [.of(j.id), .text(j.kind.rawValue), .of(j.parentId), .text(j.state.rawValue), .of(j.outcome?.rawValue), .json(j.error.map(errorJson)),
     .json(j.blockedBy), .json(j.decision), .text(j.queue.rawValue), .of(j.position), .of(j.driveId), .of(j.mediaGeneration), .of(j.automatic),
     .of(j.mode), .text(j.title), .of(j.fingerprint), .json(requestJson(j.request)), .json(j.plan.map(planJson)), .of(j.unitId), .of(j.createdAt),
     .of(j.startedAt), .of(j.finishedAt)]
}

private func mapJob(_ r: Row) throws(BroError) -> JobRecord {
    guard let request = r.json(16) else { throw SqliteStore.failed("read", "jobs.request") }
    return JobRecord(id: r.id(0), kind: try r.wire(1), parentId: r.idOrNil(2), state: try r.wire(3), outcome: try r.wireOrNil(4),
                     error: r.json(5).map(errorOf), blockedBy: r.json(6), decision: r.json(7), queue: try r.wire(8), position: r.int(9),
                     driveId: r.textOrNil(10), mediaGeneration: r.int64OrNil(11), automatic: r.bool(12), mode: r.textOrNil(13), title: r.text(14),
                     fingerprint: r.textOrNil(15), request: try requestOf(request), plan: try r.json(17).map { (p) throws(BroError) in try planOf(p) },
                     unitId: r.idOrNil(18), createdAt: try r.time(19), startedAt: try r.timeOrNil(20), finishedAt: try r.timeOrNil(21))
}

private struct JobRepo: JobRepository {
    let db: SqliteStore.Db
    init(_ db: SqliteStore.Db) { self.db = db }

    func insert(_ job: JobRecord) async throws(BroError) {
        try await db.run { s throws(BroError) in
            try s.execute("INSERT INTO jobs (\(jobColumns)) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)", jobArgs(job))
        }
    }

    func update(_ job: JobRecord) async throws(BroError) {
        var args = jobArgs(job)
        args.append(args.removeFirst())
        try await db.run { s throws(BroError) in
            try s.execute("UPDATE jobs SET kind = ?, parent_id = ?, state = ?, outcome = ?, error = ?, blocked_by = ?, decision = ?, queue = ?, position = ?, "
                          + "drive_id = ?, media_generation = ?, automatic = ?, mode = ?, title = ?, fingerprint = ?, request = ?, plan = ?, unit_id = ?, "
                          + "created_at = ?, started_at = ?, finished_at = ? WHERE id = ?", args)
        }
    }

    func load(_ jobId: Id) async throws(BroError) -> JobRecord? {
        try await db.run { s throws(BroError) in try s.rows("SELECT \(jobColumns) FROM jobs WHERE id = ?", [.of(jobId)], mapJob).first }
    }

    func active() async throws(BroError) -> [JobRecord] {
        try await db.run { s throws(BroError) in
            try s.rows("SELECT \(jobColumns) FROM jobs WHERE state <> 'finished' ORDER BY "
                       + "CASE queue WHEN 'acquisition' THEN 0 WHEN 'processing' THEN 1 ELSE 2 END, position, created_at, id", [], mapJob)
        }
    }

    func query(_ q: JobQuery) async throws(BroError) -> [JobRecord] {
        let state = Value.of(q.state?.rawValue), kind = Value.of(q.kind?.rawValue), after = Value.of(q.finishedAfter)
        return try await db.run { s throws(BroError) in
            try s.rows("SELECT \(jobColumns) FROM jobs WHERE (?1 IS NULL OR state = ?1) AND (?2 IS NULL OR kind = ?2) AND (?3 IS NULL OR finished_at > ?3) "
                       + "ORDER BY created_at DESC, id DESC LIMIT ?4 OFFSET ?5", [state, kind, after, .of(q.limit), .of(q.offset)], mapJob)
        }
    }
}

// ---- steps ----------------------------------------------------------------------------------------

private let stepColumns = "job_id, seq, kind, state, attempt, started_at, finished_at, output, error, checkpoint"

private func mapStep(_ r: Row) throws(BroError) -> StepRecord {
    StepRecord(jobId: r.id(0), seq: r.int(1), kind: try r.wire(2), state: try r.wire(3), attempt: r.int(4), startedAt: try r.timeOrNil(5),
               finishedAt: try r.timeOrNil(6), output: try r.json(7).map { (o) throws(BroError) in try outputOf(o) }, error: r.json(8).map(errorOf),
               checkpoint: r.json(9))
}

private struct StepRepo: StepRepository {
    let db: SqliteStore.Db
    init(_ db: SqliteStore.Db) { self.db = db }

    func save(_ s: StepRecord) async throws(BroError) {
        try await db.run { st throws(BroError) in
            try st.execute("INSERT OR REPLACE INTO job_steps (\(stepColumns)) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                           [.of(s.jobId), .of(s.seq), .text(s.kind.rawValue), .text(s.state.rawValue), .of(s.attempt), .of(s.startedAt),
                            .of(s.finishedAt), .json(s.output.map(outputJson)), .json(s.error.map(errorJson)), .json(s.checkpoint)])
        }
    }

    func completed(_ jobId: Id) async throws(BroError) -> [StepRecord] {
        try await db.run { s throws(BroError) in
            try s.rows("SELECT \(stepColumns) FROM job_steps WHERE job_id = ? AND state = 'succeeded' ORDER BY seq", [.of(jobId)], mapStep)
        }
    }

    func load(_ jobId: Id) async throws(BroError) -> [StepRecord] {
        try await db.run { s throws(BroError) in try s.rows("SELECT \(stepColumns) FROM job_steps WHERE job_id = ? ORDER BY seq", [.of(jobId)], mapStep) }
    }
}

// ---- units ----------------------------------------------------------------------------------------

private let unitColumns = "u.id, u.library_id, u.physical_disc_id, u.job_id, u.path, u.record_file, u.record_version, u.state, u.status, u.name, u.kind, "
    + "u.format, u.format_code, u.encrypted, u.fingerprint, u.label, u.season, u.part, u.volume, u.disc, u.makemkv_version, u.bytes, u.file_count, "
    + "u.attempts, u.created_at, u.committed_at"

private func mapUnit(_ r: Row) throws(BroError) -> UnitRecord {
    UnitRecord(id: r.id(0), libraryId: r.text(1), physicalDiscId: r.idOrNil(2), jobId: r.idOrNil(3), path: r.text(4), recordFile: r.text(5),
               recordVersion: r.int(6), state: try r.wire(7), status: try r.wire(8), name: r.text(9), kind: try r.wire(10), format: r.text(11),
               formatCode: r.text(12), encrypted: r.bool(13), fingerprint: r.textOrNil(14), label: r.text(15), season: r.intOrNil(16),
               part: r.intOrNil(17), volume: r.intOrNil(18), disc: r.intOrNil(19), makemkvVersion: r.text(20), bytes: r.int64(21),
               fileCount: r.int(22), attempts: r.int(23), createdAt: try r.time(24), committedAt: try r.timeOrNil(25))
}

private let unitUpsert = "INSERT INTO archive_units (id, library_id, physical_disc_id, job_id, path, record_file, record_version, state, status, name, "
    + "kind, format, format_code, encrypted, fingerprint, label, season, part, volume, disc, makemkv_version, bytes, file_count, attempts, created_at, "
    + "committed_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
    + "ON CONFLICT (id) DO UPDATE SET library_id = excluded.library_id, physical_disc_id = excluded.physical_disc_id, job_id = excluded.job_id, "
    + "path = excluded.path, record_file = excluded.record_file, record_version = excluded.record_version, state = excluded.state, "
    + "status = excluded.status, name = excluded.name, kind = excluded.kind, format = excluded.format, format_code = excluded.format_code, "
    + "encrypted = excluded.encrypted, fingerprint = excluded.fingerprint, label = excluded.label, season = excluded.season, part = excluded.part, "
    + "volume = excluded.volume, disc = excluded.disc, makemkv_version = excluded.makemkv_version, bytes = excluded.bytes, "
    + "file_count = excluded.file_count, attempts = excluded.attempts, created_at = excluded.created_at, committed_at = excluded.committed_at"

private func unitArgs(_ u: UnitRecord) -> [Value] {
    [.of(u.id), .text(u.libraryId), .of(u.physicalDiscId), .of(u.jobId), .text(u.path), .text(u.recordFile), .of(u.recordVersion),
     .text(u.state.rawValue), .text(u.status.rawValue), .text(u.name), .text(u.kind.rawValue), .text(u.format), .text(u.formatCode),
     .of(u.encrypted), .of(u.fingerprint), .text(u.label), .of(u.season), .of(u.part), .of(u.volume), .of(u.disc), .text(u.makemkvVersion),
     .of(u.bytes), .of(u.fileCount), .of(u.attempts), .of(u.createdAt), .of(u.committedAt)]
}

private struct UnitRepo: UnitRepository {
    let db: SqliteStore.Db
    init(_ db: SqliteStore.Db) { self.db = db }

    func beginCommit(_ unit: UnitRecord, intent: CommitIntent) async throws(BroError) {
        try await db.atomic { s throws(BroError) in
            try s.execute(unitUpsert, unitArgs(unit))
            try s.execute("INSERT INTO commit_intents (unit_id, job_id, staging, destination, merge, quarantine, created_at) VALUES (?, ?, ?, ?, ?, ?, ?)",
                          [.of(intent.unitId), .of(intent.jobId), .text(intent.staging), .text(intent.destination), .of(intent.merge),
                           .of(intent.quarantine), .of(intent.createdAt)])
            for i in intent.items {
                try s.execute("INSERT INTO commit_items (unit_id, seq, from_path, to_path, sha256, is_dir, moved) VALUES (?, ?, ?, ?, ?, ?, ?)",
                              [.of(intent.unitId), .of(i.seq), .text(i.fromPath), .text(i.toPath), .of(i.sha256), .of(i.isDir), .of(i.moved)])
            }
        }
    }

    func markMoved(_ unitId: Id, seq: Int) async throws(BroError) {
        try await db.run { s throws(BroError) in try s.execute("UPDATE commit_items SET moved = 1 WHERE unit_id = ? AND seq = ?", [.of(unitId), .of(seq)]) }
    }

    func finishCommit(_ intent: CommitIntent, record: UnitRecord, files: [UnitFile]) async throws(BroError) {
        try await db.atomic { s throws(BroError) in
            try s.execute(unitUpsert, unitArgs(record))
            try s.execute("DELETE FROM unit_files WHERE unit_id = ?", [.of(record.id)])
            for f in files {
                try s.execute("INSERT INTO unit_files (unit_id, path, size, sha256, role, title, episode) VALUES (?, ?, ?, ?, ?, ?, ?)",
                              [.of(record.id), .text(f.path), .of(f.size), .text(f.sha256), .text(f.role), .of(f.title), .of(f.episode)])
            }
            try s.execute("DELETE FROM commit_intents WHERE unit_id = ?", [.of(intent.unitId)])
        }
    }

    func openIntents() async throws(BroError) -> [CommitIntent] {
        try await db.run { s throws(BroError) in
            let heads = try s.rows("SELECT unit_id, job_id, staging, destination, merge, quarantine, created_at FROM commit_intents ORDER BY created_at, unit_id",
                                   []) { (r) throws(BroError) in
                (unit: r.id(0), job: r.id(1), staging: r.text(2), destination: r.text(3), merge: r.bool(4), quarantine: r.bool(5), created: try r.time(6))
            }
            var out: [CommitIntent] = []
            for h in heads {
                let items = try s.rows("SELECT seq, from_path, to_path, sha256, is_dir, moved FROM commit_items WHERE unit_id = ? ORDER BY seq", [.of(h.unit)]) {
                    CommitItem(seq: $0.int(0), fromPath: $0.text(1), toPath: $0.text(2), sha256: $0.textOrNil(3), isDir: $0.bool(4), moved: $0.bool(5))
                }
                out.append(CommitIntent(unitId: h.unit, jobId: h.job, staging: h.staging, destination: h.destination, merge: h.merge,
                                        quarantine: h.quarantine, createdAt: h.created, items: items))
            }
            return out
        }
    }

    func get(_ unitId: Id) async throws(BroError) -> UnitRecord? {
        try await db.run { s throws(BroError) in try s.rows("SELECT \(unitColumns) FROM archive_units u WHERE u.id = ?", [.of(unitId)], mapUnit).first }
    }

    func query(_ q: UnitQuery) async throws(BroError) -> [UnitRecord] {
        try await db.run { s throws(BroError) in
            try s.rows("SELECT \(unitColumns) FROM archive_units u WHERE (?1 IS NULL OR u.library_id = ?1) AND (?2 IS NULL OR instr(lower(u.name), lower(?2)) > 0) "
                       + "AND (?3 IS NULL OR u.state = ?3) ORDER BY u.created_at DESC, u.id DESC LIMIT ?4 OFFSET ?5",
                       [.of(q.libraryId), .of(q.text), .of(q.state?.rawValue), .of(q.limit), .of(q.offset)], mapUnit)
        }
    }

    func leastRecentlyVerified(_ limit: Int) async throws(BroError) -> [UnitRecord] {
        try await db.run { s throws(BroError) in
            try s.rows("SELECT \(unitColumns) FROM archive_units u WHERE u.state = 'committed' "
                       + "ORDER BY (SELECT max(c.finished_at) FROM checks c WHERE c.unit_id = u.id) IS NOT NULL, "
                       + "(SELECT max(c.finished_at) FROM checks c WHERE c.unit_id = u.id), u.created_at, u.id LIMIT ?", [.of(limit)], mapUnit)
        }
    }

    func markMissing(_ unitId: Id) async throws(BroError) {
        try await db.run { s throws(BroError) in try s.execute("UPDATE archive_units SET state = 'missing' WHERE id = ?", [.of(unitId)]) }
    }
}

// ---- catalogue ------------------------------------------------------------------------------------

private struct CatalogRepo: CatalogRepository {
    let db: SqliteStore.Db
    init(_ db: SqliteStore.Db) { self.db = db }

    func archivedBefore(_ fingerprint: String) async throws(BroError) -> [UnitRecord] {
        try await db.run { s throws(BroError) in
            try s.rows("SELECT \(unitColumns) FROM archive_units u WHERE u.fingerprint = ? AND u.state = 'committed' ORDER BY u.committed_at DESC, u.id",
                       [.text(fingerprint)], mapUnit)
        }
    }

    /// Committed TV discs (success or errors), as EpisodeContinuation sees them; only disc `disc` when given.
    private func candidates(_ s: SqliteStore, disc: Int?) throws(BroError) -> [ArchivedDisc] {
        try s.rows("SELECT u.name, u.label, u.season, u.part, u.volume, u.disc, l.path, u.path, "
                   + "(SELECT max(f.episode) FROM unit_files f WHERE f.unit_id = u.id) FROM archive_units u JOIN libraries l ON l.id = u.library_id "
                   + "WHERE u.kind = 'tv' AND u.state = 'committed' AND u.status IN ('success', 'errors') AND (?1 IS NULL OR u.disc = ?1) "
                   + "ORDER BY u.committed_at, u.id", [.of(disc)]) { r in
            var library = r.text(6)
            while library.hasSuffix("/") { library.removeLast() }
            return ArchivedDisc(name: r.text(0), labelTitle: LabelParser.parse(r.text(1)).title, season: r.intOrNil(2), part: r.intOrNil(3),
                                volume: r.intOrNil(4), disc: r.intOrNil(5), lastEpisode: r.intOrNil(8), folder: library + "/" + r.text(7))
        }
    }

    func previousDisc(_ query: ContinuationQuery) async throws(BroError) -> ArchivedDisc? {
        guard query.disc > 1 else { return nil }
        return try await db.run { s throws(BroError) in
            try candidates(s, disc: query.disc - 1).filter { EpisodeContinuation.sameSet($0, query: query) && $0.lastEpisode != nil }
                .max { ($0.lastEpisode ?? 0) < ($1.lastEpisode ?? 0) }
        }
    }

    func highestEpisode(_ query: ContinuationQuery) async throws(BroError) -> Int? {
        try await db.run { s throws(BroError) in
            try candidates(s, disc: nil).filter { EpisodeContinuation.sameSet($0, query: query) }.compactMap(\.lastEpisode).max()
        }
    }

    func works(_ query: String) async throws(BroError) -> [WorkRecord] {
        try await db.run { s throws(BroError) in
            try s.rows("SELECT id, kind, title, year, tmdb_id, imdb_id, created_at, updated_at FROM works WHERE instr(lower(title), lower(?)) > 0 "
                       + "ORDER BY title COLLATE NOCASE, id", [.text(query)]) { (r) throws(BroError) in
                WorkRecord(id: r.id(0), kind: try r.wire(1), title: r.text(2), year: r.intOrNil(3), tmdbId: r.intOrNil(4), imdbId: r.textOrNil(5),
                           createdAt: try r.time(6), updatedAt: try r.time(7))
            }
        }
    }

    func set(_ setId: Id) async throws(BroError) -> DiscSetRecord? {
        try await db.run { s throws(BroError) in
            try s.rows("SELECT id, work_id, label_title, season, part, volume, description, known_count FROM disc_sets WHERE id = ?", [.of(setId)]) {
                DiscSetRecord(id: $0.id(0), workId: $0.idOrNil(1), labelTitle: $0.text(2), season: $0.intOrNil(3), part: $0.intOrNil(4),
                              volume: $0.intOrNil(5), description: $0.text(6), knownCount: $0.intOrNil(7))
            }.first
        }
    }
}

// ---- checks, replicas, drives ---------------------------------------------------------------------

private let checkColumns = "id, unit_id, replica_id, folder, job_id, started_at, finished_at, result, files, bytes, changed, unreadable, missing, unlisted, error"

private func mapCheck(_ r: Row) throws(BroError) -> CheckRecord {
    CheckRecord(id: r.id(0), unitId: r.idOrNil(1), replicaId: r.idOrNil(2), folder: r.text(3), jobId: r.idOrNil(4), startedAt: try r.time(5),
                finishedAt: try r.timeOrNil(6), result: try r.wire(7), files: r.int(8), bytes: r.int64(9), changed: stringsOf(r.json(10)),
                unreadable: stringsOf(r.json(11)), missing: stringsOf(r.json(12)), unlisted: stringsOf(r.json(13)), error: r.json(14).map(messageOf))
}

private struct CheckRepo: CheckRepository {
    let db: SqliteStore.Db
    init(_ db: SqliteStore.Db) { self.db = db }

    func insert(_ c: CheckRecord) async throws(BroError) {
        try await db.run { s throws(BroError) in
            try s.execute("INSERT INTO checks (\(checkColumns)) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                          [.of(c.id), .of(c.unitId), .of(c.replicaId), .text(c.folder), .of(c.jobId), .of(c.startedAt), .of(c.finishedAt),
                           .text(c.result.rawValue), .of(c.files), .of(c.bytes), .json(strings(c.changed)), .json(strings(c.unreadable)),
                           .json(strings(c.missing)), .json(strings(c.unlisted)), .json(c.error.map(messageJson))])
        }
    }

    func forUnit(_ unitId: Id) async throws(BroError) -> [CheckRecord] {
        try await db.run { s throws(BroError) in
            try s.rows("SELECT \(checkColumns) FROM checks WHERE unit_id = ? ORDER BY started_at DESC, id DESC", [.of(unitId)], mapCheck)
        }
    }

    func latest(_ folder: String) async throws(BroError) -> CheckRecord? {
        try await db.run { s throws(BroError) in
            try s.rows("SELECT \(checkColumns) FROM checks WHERE folder = ? ORDER BY coalesce(finished_at, started_at) DESC, id DESC LIMIT 1",
                       [.text(folder)], mapCheck).first
        }
    }
}

private func mapReplica(_ r: Row) throws(BroError) -> ReplicaRecord {
    ReplicaRecord(id: r.id(0), unitId: r.id(1), targetId: r.text(2), path: r.text(3), state: try r.wire(4), verifiedAt: try r.timeOrNil(5),
                  error: r.json(6).map(messageOf))
}

private struct ReplicaRepo: ReplicaRepository {
    let db: SqliteStore.Db
    init(_ db: SqliteStore.Db) { self.db = db }

    func upsert(_ x: ReplicaRecord) async throws(BroError) {
        try await db.run { s throws(BroError) in
            try s.execute("INSERT INTO replicas (id, unit_id, target_id, path, state, verified_at, error) VALUES (?, ?, ?, ?, ?, ?, ?) "
                          + "ON CONFLICT (id) DO UPDATE SET unit_id = excluded.unit_id, target_id = excluded.target_id, path = excluded.path, "
                          + "state = excluded.state, verified_at = excluded.verified_at, error = excluded.error",
                          [.of(x.id), .of(x.unitId), .text(x.targetId), .text(x.path), .text(x.state.rawValue), .of(x.verifiedAt),
                           .json(x.error.map(messageJson))])
        }
    }

    func forUnit(_ unitId: Id) async throws(BroError) -> [ReplicaRecord] {
        try await db.run { s throws(BroError) in
            try s.rows("SELECT id, unit_id, target_id, path, state, verified_at, error FROM replicas WHERE unit_id = ? ORDER BY target_id, id",
                       [.of(unitId)], mapReplica)
        }
    }

    func lagging() async throws(BroError) -> [ReplicaRecord] {
        try await db.run { s throws(BroError) in
            try s.rows("SELECT id, unit_id, target_id, path, state, verified_at, error FROM replicas WHERE state <> 'verified' ORDER BY id", [], mapReplica)
        }
    }
}

private struct DriveRepo: DriveRepository {
    let db: SqliteStore.Db
    init(_ db: SqliteStore.Db) { self.db = db }

    func upsert(_ d: DriveRecord) async throws(BroError) {
        try await db.run { s throws(BroError) in
            try s.execute("INSERT INTO drives (id, identification, model, last_device, config_id, first_seen_at, last_seen_at) VALUES (?, ?, ?, ?, ?, ?, ?) "
                          + "ON CONFLICT (id) DO UPDATE SET identification = excluded.identification, model = excluded.model, "
                          + "last_device = excluded.last_device, config_id = excluded.config_id, last_seen_at = excluded.last_seen_at",
                          [.text(d.id), .text(d.identification), .text(d.model), .text(d.lastDevice), .of(d.configId), .of(d.firstSeenAt), .of(d.lastSeenAt)])
        }
    }

    func all() async throws(BroError) -> [DriveRecord] {
        try await db.run { s throws(BroError) in
            try s.rows("SELECT id, identification, model, last_device, config_id, first_seen_at, last_seen_at FROM drives ORDER BY id", []) {
                (r) throws(BroError) in
                DriveRecord(id: r.text(0), identification: r.text(1), model: r.text(2), lastDevice: r.text(3), configId: r.textOrNil(4),
                            firstSeenAt: try r.time(5), lastSeenAt: try r.time(6))
            }
        }
    }

    func recordStats(_ driveId: String, day: String, stats st: DriveStats) async throws(BroError) {
        try await db.run { s throws(BroError) in
            try s.execute("INSERT INTO drive_stats_daily (drive_id, day, jobs, failed_jobs, read_error_jobs, read_errors, bytes_read, seconds_reading) "
                          + "VALUES (?, ?, ?, ?, ?, ?, ?, ?) ON CONFLICT (drive_id, day) DO UPDATE SET jobs = jobs + excluded.jobs, "
                          + "failed_jobs = failed_jobs + excluded.failed_jobs, read_error_jobs = read_error_jobs + excluded.read_error_jobs, "
                          + "read_errors = read_errors + excluded.read_errors, bytes_read = bytes_read + excluded.bytes_read, "
                          + "seconds_reading = seconds_reading + excluded.seconds_reading",
                          [.text(driveId), .text(day), .of(st.jobs), .of(st.failedJobs), .of(st.readErrorJobs), .of(st.readErrors), .of(st.bytesRead),
                           .double(st.secondsReading)])
        }
    }
}

// ---- services -------------------------------------------------------------------------------------

private struct LookupRepo: LookupCacheRepository {
    let db: SqliteStore.Db
    init(_ db: SqliteStore.Db) { self.db = db }

    func get(_ provider: String, key: String) async throws(BroError) -> HttpResponse? {
        let now = db.now
        return try await db.run { s throws(BroError) in
            try s.rows("SELECT status, response FROM lookup_cache WHERE provider = ? AND request_key = ? AND expires_at > ?",
                       [.text(provider), .text(key), .of(now)]) { HttpResponse(status: $0.int(0), headers: [], body: $0.blob(1)) }.first
        }
    }

    func put(_ provider: String, key: String, response: HttpResponse, expiresAt: Instant) async throws(BroError) {
        let now = db.now
        try await db.run { s throws(BroError) in
            try s.execute("INSERT OR REPLACE INTO lookup_cache (provider, request_key, status, response, fetched_at, expires_at) VALUES (?, ?, ?, ?, ?, ?)",
                          [.text(provider), .text(key), .of(response.status), .blob(response.body), .of(now), .of(expiresAt)])
        }
    }
}

private let outboxColumns = "id, target_id, job_id, title, body, status, created_at, attempts, next_attempt_at, sent_at, last_error"

private func mapOutbox(_ r: Row) throws(BroError) -> OutboxEntry {
    guard let title = r.json(3) else { throw SqliteStore.failed("read", "outbox.title") }
    return OutboxEntry(id: r.id(0), targetId: r.text(1), jobId: r.idOrNil(2), title: messageOf(title), lines: (r.json(4)?.array ?? []).map(messageOf),
                       status: try r.wire(5), createdAt: try r.time(6), attempts: r.int(7), nextAttemptAt: try r.timeOrNil(8), sentAt: try r.timeOrNil(9),
                       lastError: r.json(10).map(errorOf))
}

private struct OutboxRepo: OutboxRepository {
    let db: SqliteStore.Db
    init(_ db: SqliteStore.Db) { self.db = db }

    func add(_ n: OutboxEntry) async throws(BroError) {
        try await db.run { s throws(BroError) in
            try s.execute("INSERT INTO outbox (\(outboxColumns)) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                          [.of(n.id), .text(n.targetId), .of(n.jobId), .json(messageJson(n.title)), .json(.array(n.lines.map(messageJson))),
                           .text(n.status.rawValue), .of(n.createdAt), .of(n.attempts), .of(n.nextAttemptAt), .of(n.sentAt), .json(n.lastError.map(errorJson))])
        }
    }

    func due(_ now: Instant) async throws(BroError) -> [OutboxEntry] {
        try await db.run { s throws(BroError) in
            try s.rows("SELECT \(outboxColumns) FROM outbox WHERE sent_at IS NULL AND next_attempt_at IS NOT NULL AND next_attempt_at <= ? "
                       + "ORDER BY next_attempt_at, created_at, id", [.of(now)], mapOutbox)
        }
    }

    func markSent(_ id: Id) async throws(BroError) {
        let now = db.now
        try await db.run { s throws(BroError) in
            try s.execute("UPDATE outbox SET sent_at = ?, attempts = attempts + 1, last_error = NULL WHERE id = ?", [.of(now), .of(id)])
        }
    }

    func markFailed(_ id: Id, error: BroError, retryAt: Instant?) async throws(BroError) {
        try await db.run { s throws(BroError) in
            try s.execute("UPDATE outbox SET attempts = attempts + 1, last_error = ?, next_attempt_at = ? WHERE id = ?",
                          [.json(errorJson(error)), .of(retryAt), .of(id)])
        }
    }
}

private struct KvRepo: KeyValueRepository {
    let db: SqliteStore.Db
    init(_ db: SqliteStore.Db) { self.db = db }

    func get(_ key: String) async throws(BroError) -> JsonValue? {
        try await db.run { s throws(BroError) in try s.rows("SELECT value FROM kv WHERE key = ?", [.text(key)]) { $0.json(0) }.first ?? nil }
    }

    func set(_ key: String, value: JsonValue) async throws(BroError) {
        let now = db.now
        try await db.run { s throws(BroError) in
            try s.execute("INSERT INTO kv (key, value, updated_at) VALUES (?, ?, ?) ON CONFLICT (key) DO UPDATE SET value = excluded.value, "
                          + "updated_at = excluded.updated_at", [.text(key), .json(value), .of(now)])
        }
    }
}

/// A first-come, first-served lock for async code: the store's one connection.
private final class AsyncLock: @unchecked Sendable {
    private let mutex = NSLock()
    private var held = false
    private var waiters: [CheckedContinuation<Void, Never>] = []

    func acquire() async {
        await withCheckedContinuation { (c: CheckedContinuation<Void, Never>) in
            mutex.lock()
            if held {
                waiters.append(c)
                mutex.unlock()
            } else {
                held = true
                mutex.unlock()
                c.resume()
            }
        }
    }

    func release() {
        mutex.lock()
        if waiters.isEmpty {
            held = false
            mutex.unlock()
        } else {
            let next = waiters.removeFirst()
            mutex.unlock()
            next.resume()
        }
    }
}
