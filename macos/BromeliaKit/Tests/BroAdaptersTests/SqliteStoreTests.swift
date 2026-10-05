import BroAdapters
import BroDomain
import BroFoundation
import BroPorts
import BroTestSupport
import Foundation
import SQLite3
import Testing

/// A clock stopped at 2026-10-04T12:00:00.000Z.
struct FixedClock: Clock {
    let at = Instant.parse("2026-10-04T12:00:00.000Z")!
    func now() -> Instant { at }
    func monotonic() -> Duration { Duration(seconds: 0) }
    func sleep(_ duration: Duration, cancel: CancellationToken) async throws(BroError) {}
    func timer(_ schedule: TimerSchedule, handler: @escaping @Sendable () -> Void) -> any TimerHandle { fatalError("not in these tests") }
}

/// A second connection to a store's file, for seeding rows and reading what was written.
final class RawDb {
    let db: OpaquePointer
    init(_ path: String) {
        var h: OpaquePointer?
        sqlite3_open(path, &h)
        db = h!
        sqlite3_exec(db, "PRAGMA foreign_keys=ON", nil, nil, nil)
    }
    deinit { sqlite3_close(db) }

    func exec(_ sql: String) { sqlite3_exec(db, sql, nil, nil, nil) }

    func insert(_ table: String, _ row: [(key: String, value: JsonValue)]) throws {
        let sql = "INSERT INTO \(table) (\(row.map(\.key).joined(separator: ", "))) VALUES (\(row.map { _ in "?" }.joined(separator: ", ")))"
        var stmt: OpaquePointer?
        guard sqlite3_prepare_v2(db, sql, -1, &stmt, nil) == SQLITE_OK else { throw FixtureError(String(cString: sqlite3_errmsg(db))) }
        defer { sqlite3_finalize(stmt) }
        let transient = unsafeBitCast(-1, to: sqlite3_destructor_type.self)
        for (i, m) in row.enumerated() {
            let idx = Int32(i + 1)
            switch m.value {
            case .null: sqlite3_bind_null(stmt, idx)
            case .string(let s) where table == "lookup_cache" && m.key == "response":
                let bytes = Array(s.utf8)
                bytes.withUnsafeBytes { _ = sqlite3_bind_blob(stmt, idx, $0.baseAddress, Int32(bytes.count), transient) }
            case .string(let s): sqlite3_bind_text(stmt, idx, s, -1, transient)
            case .integer(let n): sqlite3_bind_int64(stmt, idx, n)
            case .number(let d): sqlite3_bind_double(stmt, idx, d)
            case .bool(let b): sqlite3_bind_int64(stmt, idx, b ? 1 : 0)
            default: throw FixtureError("\(m.key): unsupported value")
            }
        }
        guard sqlite3_step(stmt) == SQLITE_DONE else { throw FixtureError(String(cString: sqlite3_errmsg(db))) }
    }

    /// Every row of a query, each column as text (NULL as nil).
    func rows(_ sql: String) -> [[String?]] {
        var stmt: OpaquePointer?
        sqlite3_prepare_v2(db, sql, -1, &stmt, nil)
        defer { sqlite3_finalize(stmt) }
        var out: [[String?]] = []
        while sqlite3_step(stmt) == SQLITE_ROW {
            out.append((0..<sqlite3_column_count(stmt)).map { i in sqlite3_column_text(stmt, i).map { String(cString: $0) } })
        }
        return out
    }

    /// The schema described as tools/check-contracts.py writes 0001_init.expected.txt.
    func schemaDump() -> String {
        var lines: [String] = []
        for t in rows("SELECT name FROM sqlite_schema WHERE type = 'table' AND name NOT LIKE 'sqlite_%' ORDER BY name") {
            let name = t[0]!
            lines.append("table \(name)")
            for c in rows("PRAGMA table_info(\(name))") {
                let notNull = c[3] == "1" ? " NOT NULL" : "", def = c[4].map { " DEFAULT \($0)" } ?? "", pk = c[5] != "0" ? " PK\(c[5]!)" : ""
                lines.append("  \(c[1]!) \(c[2]!)\(notNull)\(def)\(pk)")
            }
            for fk in rows("PRAGMA foreign_key_list(\(name))") { lines.append("  fk \(fk[3]!) -> \(fk[2]!)(\(fk[4]!)) on delete \(fk[6]!.lowercased())") }
        }
        for i in rows("SELECT name, tbl_name, sql FROM sqlite_schema WHERE type = 'index' AND sql IS NOT NULL ORDER BY name") {
            lines.append("index \(i[0]!) on \(i[1]!): \(i[2]!.split(whereSeparator: { $0 == " " || $0 == "\n" || $0 == "\t" }).joined(separator: " "))")
        }
        lines.append("user_version \(rows("PRAGMA user_version")[0][0]!)")
        return lines.joined(separator: "\n") + "\n"
    }
}

/// shared/fixtures/adapters/store.cases.json, and every record type read back equal.
@Suite(.serialized) struct SqliteStoreTests {
    let dir = NSTemporaryDirectory() + "bromelia-store-" + UUID().uuidString

    init() throws { try FileManager.default.createDirectory(atPath: dir, withIntermediateDirectories: true) }

    func store(_ name: String) async throws -> SqliteStore {
        let s = try SqliteStore.open(dir + "/" + name + ".sqlite", clock: FixedClock())
        try await s.migrate()
        return s
    }

    func continuation(_ a: JsonValue) -> ContinuationQuery {
        ContinuationQuery(name: a["name"]!.string!, labelTitle: a["labelTitle"]!.string!, season: a["season"]?.int.map { Int($0) },
                          part: a["part"]?.int.map { Int($0) }, volume: a["volume"]?.int.map { Int($0) }, disc: Int(a["disc"]!.int!))
    }

    @Test func theSharedCasesPass() async throws {
        defer { try? FileManager.default.removeItem(atPath: dir) }
        let doc = try Fixtures.json("adapters/store.cases.json")
        var failures: [String] = []
        for (n, c) in (doc["cases"]?.array ?? []).enumerated() {
            let id = c["id"]!.string!
            do {
                try await run("case\(n)", c["given"]!, c["expect"]!)
            } catch {
                failures.append("\(id): \(error)")
            }
        }
        #expect(failures.isEmpty, "\(failures.joined(separator: "\n"))")
    }

    func run(_ name: String, _ given: JsonValue, _ expect: JsonValue) async throws {
        let s = try await store(name)
        defer { s.close() }
        let raw = RawDb(dir + "/" + name + ".sqlite")
        for (table, rows) in given["rows"]?.members ?? [] {
            for row in rows.array ?? [] { try raw.insert(table, row.members ?? []) }
        }
        let a = given["args"] ?? .object([])
        func same(_ key: String, _ actual: JsonValue) throws { try Fixtures.same(expect[key] ?? .null, actual, key) }
        func ids(_ list: [String]) -> JsonValue { .array(list.map { .string($0) }) }
        func n(_ x: Int?) -> JsonValue { x.map { .integer(Int64($0)) } ?? .null }
        switch given["call"]!.string! {
        case "jobs.active": try same("ids", ids(try await s.jobs().active().map(\.id.value)))
        case "jobs.query":
            let q = JobQuery(state: a["state"]?.string.flatMap(JobState.init(rawValue:)), kind: a["kind"]?.string.flatMap(JobKind.init(rawValue:)),
                             finishedAfter: a["finishedAfter"]?.string.flatMap(Instant.parse), limit: Int(a["limit"]!.int!), offset: Int(a["offset"]!.int!))
            try same("ids", ids(try await s.jobs().query(q).map(\.id.value)))
        case "steps.completed": try same("seqs", .array(try await s.steps().completed(Id(a["jobId"]!.string!)).map { .integer(Int64($0.seq)) }))
        case "steps.load": try same("seqs", .array(try await s.steps().load(Id(a["jobId"]!.string!)).map { .integer(Int64($0.seq)) }))
        case "units.query":
            let q = UnitQuery(libraryId: a["libraryId"]?.string, text: a["text"]?.string, state: a["state"]?.string.flatMap(UnitState.init(rawValue:)),
                              limit: Int(a["limit"]!.int!), offset: Int(a["offset"]!.int!))
            try same("ids", ids(try await s.units().query(q).map(\.id.value)))
        case "units.leastRecentlyVerified": try same("ids", ids(try await s.units().leastRecentlyVerified(Int(a["limit"]!.int!)).map(\.id.value)))
        case "units.openIntents":
            try same("intents", .array(try await s.units().openIntents().map { i in
                .array([.string(i.unitId.value), .array(i.items.map { .string("\($0.seq) \($0.fromPath) \($0.toPath)\($0.isDir ? " dir" : "")\($0.moved ? " moved" : "")") })])
            }))
        case "catalog.archivedBefore": try same("ids", ids(try await s.catalog().archivedBefore(a["fingerprint"]!.string!).map(\.id.value)))
        case "catalog.previousDisc":
            let p = try await s.catalog().previousDisc(continuation(a))
            try same("previous", p.map { .object([("name", .string($0.name)), ("disc", n($0.disc)), ("lastEpisode", n($0.lastEpisode)), ("folder", .string($0.folder))]) } ?? .null)
        case "catalog.highestEpisode": try same("episode", n(try await s.catalog().highestEpisode(continuation(a))))
        case "catalog.works": try same("ids", ids(try await s.catalog().works(a["query"]!.string!).map(\.id.value)))
        case "catalog.set":
            let set = try #require(try await s.catalog().set(Id(a["setId"]!.string!)))
            try same("set", .object([("workId", set.workId.map { .string($0.value) } ?? .null), ("labelTitle", .string(set.labelTitle)), ("season", n(set.season)),
                                     ("part", n(set.part)), ("volume", n(set.volume)), ("description", .string(set.description)), ("knownCount", n(set.knownCount))]))
        case "checks.forUnit":
            let checks = try await s.checks().forUnit(Id(a["unitId"]!.string!))
            try same("ids", ids(checks.map(\.id.value)))
            try same("changed", .array(checks[0].changed.map { .string($0) }))
            try Fixtures.same(expect["latest"]?.string, try await s.checks().latest(expect["latestFolder"]!.string!)?.id.value, "latest")
        case "replicas.lagging": try same("ids", ids(try await s.replicas().lagging().map(\.id.value)))
        case "outbox.due": try same("ids", ids(try await s.outbox().due(Instant.parse(a["now"]!.string!)!).map(\.id.value)))
        case "lookupCache.get":
            var bodies: [JsonValue] = []
            for k in a["keys"]?.array ?? [] {
                bodies.append(try await s.lookupCache().get(a["provider"]!.string!, key: k.string!).map { .string(String(decoding: $0.body, as: UTF8.self)) } ?? .null)
            }
            try same("bodies", .array(bodies))
        case "drives.recordStats":
            func stats(_ v: JsonValue) -> DriveStats {
                DriveStats(jobs: Int(v["jobs"]!.int!), failedJobs: Int(v["failedJobs"]!.int!), readErrorJobs: Int(v["readErrorJobs"]!.int!),
                           readErrors: Int(v["readErrors"]!.int!), bytesRead: v["bytesRead"]!.int!, secondsReading: v["secondsReading"]!.double!)
            }
            try await s.drives().recordStats(a["driveId"]!.string!, day: a["day"]!.string!, stats: stats(a["twice"]!))
            try await s.drives().recordStats(a["driveId"]!.string!, day: a["day"]!.string!, stats: stats(a["then"]!))
            let r = raw.rows("SELECT jobs, failed_jobs, read_error_jobs, read_errors, bytes_read, seconds_reading FROM drive_stats_daily")[0]
            try same("row", .object([("jobs", .integer(Int64(r[0]!)!)), ("failed_jobs", .integer(Int64(r[1]!)!)), ("read_error_jobs", .integer(Int64(r[2]!)!)),
                                     ("read_errors", .integer(Int64(r[3]!)!)), ("bytes_read", .integer(Int64(r[4]!)!)), ("seconds_reading", .number(Double(r[5]!)!))]))
        default: throw FixtureError("no test handles this call")
        }
    }

    static let t1 = Instant.parse("2026-10-01T12:00:00.000Z")!
    static let t2 = Instant.parse("2026-10-02T12:00:00.123Z")!
    static func u(_ n: Int) -> Id { Id(String(format: "00000000-0000-4000-8000-%012d", n)) }

    @Test func everyRecordReadsBackEqual() async throws {
        defer { try? FileManager.default.removeItem(atPath: dir) }
        let s = try await store("db")
        defer { s.close() }
        let u = Self.u, t1 = Self.t1, t2 = Self.t2
        let job = JobRecord(id: u(1), kind: .videoDisc, state: .running, outcome: .failed,
                            error: BroError("process.couldNotStart", [("tool", .string("makemkvcon"))], cause: BroError("fs.notFound", [("path", .string("/x"))])),
                            blockedBy: .object([("drive", .string("d1"))]), queue: .acquisition, position: 2, driveId: "d1", mediaGeneration: 3, automatic: true,
                            mode: "mkv", title: "Disc", fingerprint: "v1:aa",
                            request: JobRequest(kind: .videoDisc, title: "Disc", automatic: true, driveId: "d1", mediaGeneration: 3, sessionId: u(9),
                                                options: .object([("x", .integer(1))])),
                            plan: JobPlan(steps: [.awaitMedia, .probe], mode: "mkv", profileId: "default", libraryId: "lib"), createdAt: t1, startedAt: t2)
        try await s.jobs().insert(job)
        #expect(try await s.jobs().load(u(1)) == job)
        var updated = job
        updated.state = .finished
        updated.finishedAt = t2
        updated.plan = nil
        updated.error = nil
        try await s.jobs().update(updated)
        #expect(try await s.jobs().load(u(1)) == updated)
        #expect(try await s.jobs().load(u(2)) == nil)

        var step = StepRecord(jobId: u(1), seq: 0, kind: .probe, state: .succeeded, attempt: 2, startedAt: t1, finishedAt: t2,
                              output: StepOutput(kind: .probe, data: .object([("titles", .integer(3))]), summary: BroMessage(.jobCancelled, severity: .warning)),
                              error: BroError("job.cancelled"), checkpoint: .object([("sector", .integer(10))]))
        try await s.steps().save(step)
        step.attempt = 3
        try await s.steps().save(step)
        #expect(try await s.steps().load(u(1)) == [step])

        let raw = RawDb(dir + "/db.sqlite")
        raw.exec("INSERT INTO libraries (id, name, path, marker_id, attached_at) VALUES ('lib', 'L', '/archive', 'm', '2026-10-01T12:00:00.000Z')")
        let unit = UnitRecord(id: u(100), libraryId: "lib", jobId: u(1), path: "Movies/Disc", recordFile: "bromelia-00000000.json", recordVersion: 3,
                              state: .committing, status: .success, name: "Disc", kind: .movie, format: "bluray", formatCode: "BD", encrypted: true,
                              fingerprint: "v1:aa", label: "DISC", disc: 1, makemkvVersion: "v2.0", bytes: 1234, fileCount: 2, attempts: 1, createdAt: t1)
        let intent = CommitIntent(unitId: u(100), jobId: u(1), staging: "/staging", destination: "/archive/Movies/Disc", merge: false, quarantine: false,
                                  createdAt: t1, items: [CommitItem(seq: 0, fromPath: "a.mkv", toPath: "a.mkv", sha256: String(repeating: "a", count: 64), isDir: false, moved: false),
                                                         CommitItem(seq: 1, fromPath: "extras", toPath: "extras", isDir: true, moved: false)])
        try await s.units().beginCommit(unit, intent: intent)
        try await s.units().markMoved(u(100), seq: 1)
        let open = try await s.units().openIntents()
        #expect(open.count == 1 && open[0].unitId == intent.unitId && open[0].items.map(\.moved) == [false, true])
        var committed = unit
        committed.state = .committed
        committed.committedAt = t2
        try await s.units().finishCommit(intent, record: committed, files: [UnitFile(path: "a.mkv", size: 1, sha256: String(repeating: "a", count: 64), role: "title", title: 0)])
        #expect(try await s.units().openIntents().isEmpty)
        #expect(try await s.units().get(u(100)) == committed)
        try await s.units().markMissing(u(100))
        #expect(try await s.units().get(u(100))?.state == .missing)

        let check = CheckRecord(id: u(200), unitId: u(100), folder: "/archive/Movies/Disc", jobId: u(1), startedAt: t1, finishedAt: t2, result: .damaged,
                                files: 2, bytes: 1234, changed: ["a.mkv"], unreadable: [], missing: ["b.mkv"], unlisted: [],
                                error: BroMessage(.fsFailed, [("path", .string("/a"))], severity: .error))
        try await s.checks().insert(check)
        #expect(try await s.checks().forUnit(u(100)) == [check])

        var replica = ReplicaRecord(id: u(300), unitId: u(100), targetId: "nas", path: "/nas/Disc", state: .copying)
        try await s.replicas().upsert(replica)
        replica.state = .verified
        replica.verifiedAt = t2
        try await s.replicas().upsert(replica)
        #expect(try await s.replicas().forUnit(u(100)) == [replica])

        var drive = DriveRecord(id: "d1", identification: "BD-RE X", model: "X", lastDevice: "/dev/rdisk4", configId: "left", firstSeenAt: t1, lastSeenAt: t1)
        try await s.drives().upsert(drive)
        var later = drive
        later.lastDevice = "/dev/rdisk5"
        later.lastSeenAt = t2
        later.firstSeenAt = t2
        try await s.drives().upsert(later)
        drive.lastDevice = "/dev/rdisk5"
        drive.lastSeenAt = t2
        #expect(try await s.drives().all() == [drive])

        try await s.lookupCache().put("tmdb", key: "k", response: HttpResponse(status: 200, headers: [], body: [1, 2, 3]), expiresAt: Instant.parse("2026-10-05T00:00:00.000Z")!)
        #expect(try await s.lookupCache().get("tmdb", key: "k")?.body == [1, 2, 3])

        let note = OutboxEntry(id: u(400), targetId: "t", jobId: u(1), title: BroMessage(.notifyTestTitle),
                               lines: [BroMessage(.notifyTestBody, [("target", .string("t"))])], status: .success, createdAt: t1, attempts: 0, nextAttemptAt: t1)
        try await s.outbox().add(note)
        #expect(try await s.outbox().due(t2) == [note])
        try await s.outbox().markFailed(u(400), error: BroError("http.failed"), retryAt: nil)
        #expect(try await s.outbox().due(t2).isEmpty)
        try await s.outbox().markSent(u(400))

        try await s.kv().set("a", value: .object([("b", .bool(true))]))
        try await s.kv().set("a", value: .object([("b", .bool(false))]))
        #expect(try await s.kv().get("a") == .object([("b", .bool(false))]))
        #expect(try await s.kv().get("nope") == nil)
    }

    struct Boom: Error {}

    @Test func aTransactionCommitsOrRollsBackAsAWhole() async throws {
        defer { try? FileManager.default.removeItem(atPath: dir) }
        let s = try await store("tx")
        defer { s.close() }
        _ = try await s.transaction { (tx) throws(BroError) in
            try await tx.kv().set("a", value: .integer(1))
            try await tx.kv().set("b", value: .integer(2))
            return 0
        }
        do {
            _ = try await s.transaction { (tx) throws(BroError) -> Int in
                try await tx.kv().set("a", value: .integer(10))
                throw BroError("job.cancelled")
            }
            Issue.record("no error")
        } catch {
            #expect(error.code == "job.cancelled")
        }
        #expect(try await s.kv().get("a") == .integer(1))
        #expect(try await s.kv().get("b") == .integer(2))
    }

    /// Inside a transaction the outer store would wait for ever for the lock the transaction holds: it fails at once
    /// instead (store.reentered), and the transaction rolls back.
    @Test func theOuterStoreInsideATransactionFails() async throws {
        defer { try? FileManager.default.removeItem(atPath: dir) }
        let s = try await store("reenter")
        defer { s.close() }
        do {
            _ = try await s.transaction { (tx) throws(BroError) -> Int in
                try await tx.kv().set("a", value: .integer(1))
                _ = try await s.kv().get("a")
                return 0
            }
            Issue.record("no error")
        } catch {
            #expect(error.code == "store.reentered")
        }
        do {
            _ = try await s.transaction { (_) throws(BroError) -> Int in try await s.transaction { (_) throws(BroError) in 0 } }
            Issue.record("no error")
        } catch {
            #expect(error.code == "store.reentered")
        }
        #expect(try await s.kv().get("a") == nil)
        // Other tasks still wait their turn, as before.
        async let a = s.transaction { (tx) throws(BroError) -> Int in
            try await tx.kv().set("b", value: .integer(2))
            return 1
        }
        async let b: Void = s.kv().set("c", value: .integer(3))
        #expect(try await a == 1)
        try await b
    }

    /// A database from a newer Bromelia isn't migrated as if it were current.
    @Test func aNewerDatabaseIsRefused() async throws {
        defer { try? FileManager.default.removeItem(atPath: dir) }
        try await store("newer").close()
        RawDb(dir + "/newer.sqlite").exec("PRAGMA user_version = 99")
        let s = try SqliteStore.open(dir + "/newer.sqlite", clock: FixedClock())
        defer { s.close() }
        do {
            try await s.migrate()
            Issue.record("no error")
        } catch {
            #expect(error.code == "store.tooNew")
        }
    }

    @Test func theSchemaIsTheSharedOneAndMigratingTwiceChangesNothing() async throws {
        defer { try? FileManager.default.removeItem(atPath: dir) }
        let s = try await store("schema")
        defer { s.close() }
        try await s.migrate()
        let raw = RawDb(dir + "/schema.sqlite")
        #expect(raw.rows("PRAGMA journal_mode")[0][0] == "wal")
        #expect(raw.schemaDump() == (try Fixtures.text("../schema/db/0001_init.expected.txt")))
        // A unit in a library the database doesn't have breaks a foreign key: store.failed, and nothing is written.
        let orphan = UnitRecord(id: Self.u(100), libraryId: "nope", path: "p", recordFile: "r", recordVersion: 3, state: .committing, status: .success,
                                name: "n", kind: .movie, format: "dvd", formatCode: "DVD", encrypted: false, label: "", makemkvVersion: "", bytes: 0,
                                fileCount: 0, attempts: 1, createdAt: Self.t1)
        do {
            try await s.units().beginCommit(orphan, intent: CommitIntent(unitId: Self.u(100), jobId: Self.u(1), staging: "/s", destination: "/d",
                                                                         merge: false, quarantine: false, createdAt: Self.t1, items: []))
            Issue.record("no error")
        } catch {
            #expect(error.code == "store.failed")
        }
        #expect(try await s.units().get(Self.u(100)) == nil)
    }
}
