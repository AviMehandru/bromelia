import BroDomain
import BroEngine
import BroFoundation
import BroPorts
import BroTestSupport
import Foundation
import Testing

private let free: Int64 = 5_000_000_000

private func failure(_ j: JsonValue) -> BroError { BroError(j["code"]!.string!, j["params"]?.members ?? []) }

private func errorJson(_ e: BroError) -> JsonValue {
    if e.code == "crash" { return .object([("crashed", e.params.first { $0.key == "point" }!.value)]) }
    var m: [(key: String, value: JsonValue)] = [("code", .string(e.code))]
    if !e.params.isEmpty { m.append(("params", .object(e.params))) }
    return .object([("error", .object(m))])
}

private func fsError(_ code: MessageCode, _ path: String) -> BroError { BroMessage(code, [("path", .string(path))], severity: .error).toError() }

private func parent(_ p: String) -> String {
    let i = p.lastIndex(of: "/")!
    return i == p.startIndex ? "/" : String(p[..<i])
}

private func lastName(_ p: String) -> String { String(p[p.index(after: p.lastIndex(of: "/")!)...]) }

private func ordinal(_ a: String, _ b: String) -> Bool { a.utf8.lexicographicallyPrecedes(b.utf8) }

/// Files and folders in memory, with the platform adapters' rules; calls can be made to fail or wait.
private final class Tree: FileSystem, @unchecked Sendable {
    private let lock = NSLock()
    var files: [String: [UInt8]] = [:]
    var dirs: Set<String> = ["/"]
    var modes: [String: Int] = [:]
    var synced: Set<String> = []
    var fails: [(op: String, path: String, error: JsonValue, times: Int)] = []
    var holds: [(op: String, path: String)] = []
    private var released = false
    private var heldCount = 0
    private let gate = DispatchSemaphore(value: 0)

    var held: Int { lock.withLock { heldCount } }

    func release() {
        lock.withLock { released = true }
        gate.signal()
    }

    private func before(_ op: String, _ path: String) throws(BroError) {
        let failed: JsonValue? = lock.withLock {
            guard let i = fails.firstIndex(where: { $0.op == op && $0.path == path && $0.times != 0 }) else { return nil }
            if fails[i].times > 0 { fails[i].times -= 1 }
            return fails[i].error
        }
        if let failed { throw failure(failed) }
        let wait = lock.withLock { () -> Bool in
            guard !released, holds.contains(where: { $0.op == op && $0.path == path }) else { return false }
            heldCount += 1
            return true
        }
        guard wait else { return }
        let ok = gate.wait(timeout: .now() + 10) == .success
        gate.signal() // released for good: let every later call through
        lock.withLock { heldCount -= 1 }
        if !ok { throw BroError("test.neverReleased") }
    }

    func makeDirs(_ dir: String) {
        lock.withLock {
            var d = dir
            while d != "/" && dirs.insert(d).inserted { d = parent(d) }
        }
    }

    func put(_ path: String, _ text: String) {
        makeDirs(parent(path))
        lock.withLock { files[path] = Array(text.utf8) }
    }

    func moveAway(_ path: String) { move(path, path + ".away") }

    private func move(_ from: String, _ to: String) {
        lock.withLock {
            for f in files.keys where f == from || f.hasPrefix(from + "/") {
                let dst = to + f.dropFirst(from.count)
                files[dst] = files.removeValue(forKey: f)
                if let mode = modes.removeValue(forKey: f) { modes[dst] = mode }
            }
            for d in dirs where d == from || d.hasPrefix(from + "/") {
                dirs.remove(d)
                dirs.insert(to + d.dropFirst(from.count))
            }
        }
    }

    func exists(_ path: String) -> Bool { lock.withLock { files[path] != nil || dirs.contains(path) } }

    func stat(_ path: String) throws(BroError) -> FileInfo {
        try before("stat", path)
        let r: FileInfo? = lock.withLock {
            if let b = files[path] { return FileInfo(size: Int64(b.count), isDirectory: false, modifiedAt: Instant(unixMilliseconds: 0)) }
            return dirs.contains(path) ? FileInfo(size: 0, isDirectory: true, modifiedAt: Instant(unixMilliseconds: 0)) : nil
        }
        guard let r else { throw fsError(.fsNotFound, path) }
        return r
    }

    func list(_ directory: String) throws(BroError) -> [DirectoryEntry] {
        try before("list", directory)
        let r: [DirectoryEntry]? = lock.withLock {
            guard dirs.contains(directory) else { return nil }
            let child = { (p: String) in p != directory && parent(p) == directory }
            return dirs.filter(child).map { DirectoryEntry(name: lastName($0), isDirectory: true) }
                + files.keys.filter(child).map { DirectoryEntry(name: lastName($0), isDirectory: false) }
        }
        guard let r else { throw fsError(.fsNotFound, directory) }
        return r
    }

    func read(_ path: String) throws(BroError) -> [UInt8] {
        try before("read", path)
        guard let b = lock.withLock({ files[path] }) else { throw fsError(.fsNotFound, path) }
        return b
    }

    func readRange(_ path: String, offset: Int64, length: Int) throws(BroError) -> [UInt8] { fatalError("not used") }

    func createDirectory(_ path: String, parentsMustExist: Bool) throws(BroError) {
        try before("createDirectory", path)
        let (isFile, isDir, parentThere) = lock.withLock { (files[path] != nil, dirs.contains(path), dirs.contains(parent(path))) }
        if isFile { throw fsError(.fsAlreadyExists, path) }
        if isDir { return }
        if parentsMustExist && !parentThere { throw fsError(.fsParentMissing, parent(path)) }
        makeDirs(path)
    }

    func writeAtomically(_ path: String, bytes: [UInt8], mode: Int) throws(BroError) {
        try before("writeAtomically", path)
        guard lock.withLock({ dirs.contains(parent(path)) }) else { throw fsError(.fsParentMissing, parent(path)) }
        lock.withLock {
            files[path] = bytes
            modes[path] = mode
        }
    }

    func rename(_ from: String, to: String) throws(BroError) {
        try before("rename", from)
        let (fromThere, toThere, parentThere) = lock.withLock {
            (files[from] != nil || dirs.contains(from), files[to] != nil || dirs.contains(to), dirs.contains(parent(to)))
        }
        if !fromThere { throw fsError(.fsNotFound, from) }
        if toThere { throw fsError(.fsAlreadyExists, to) }
        if !parentThere { throw fsError(.fsParentMissing, parent(to)) }
        move(from, to)
    }

    func moveMerging(_ from: String, to: String, policy: MovePolicy, onMoved: (MovedItem) -> Void) throws(BroError) -> [MovedItem] {
        try before("moveMerging", from)
        if !exists(from) { throw fsError(.fsNotFound, from) }
        if !(try stat(from).isDirectory) {
            throw BroMessage(.fsFailed, [("operation", .string("move")), ("path", .string(from)), ("reason", .string("it isn't a folder"))], severity: .error).toError()
        }
        if !exists(to) { try createDirectory(to, parentsMustExist: true) } else if !(try stat(to).isDirectory) { throw fsError(.fsAlreadyExists, to) }
        var moved: [MovedItem] = []
        try merge(from, to) { m in
            moved.append(m)
            onMoved(m)
        }
        return moved.sorted { ordinal($0.from, $1.from) }
    }

    private func merge(_ source: String, _ target: String, _ moved: (MovedItem) -> Void) throws(BroError) {
        for e in try list(source).sorted(by: { ordinal($0.name, $1.name) }) {
            let src = source + "/" + e.name
            let existing = try list(target)
            if e.isDirectory, let same = existing.first(where: { $0.isDirectory && $0.name.lowercased() == e.name.lowercased() }) {
                try merge(src, target + "/" + same.name, moved)
                try remove(src)
                continue
            }
            let dst = target + "/" + ConflictNamer.next(e.name, existing: existing.map(\.name), isFolder: e.isDirectory)
            try rename(src, to: dst)
            if !e.isDirectory {
                moved(MovedItem(from: src, to: dst))
            } else {
                for f in lock.withLock({ files.keys.filter { $0.hasPrefix(dst + "/") } }).sorted(by: ordinal) {
                    moved(MovedItem(from: src + f.dropFirst(dst.count), to: f))
                }
            }
        }
    }

    func remove(_ path: String) throws(BroError) {
        try before("remove", path)
        let result: Int = lock.withLock {
            if files.removeValue(forKey: path) != nil { return 0 }
            if !dirs.contains(path) { return 1 }
            if files.keys.contains(where: { $0.hasPrefix(path + "/") }) || dirs.contains(where: { $0.hasPrefix(path + "/") }) { return 2 }
            dirs.remove(path)
            return 0
        }
        if result == 1 { throw fsError(.fsNotFound, path) }
        if result == 2 {
            throw BroMessage(.fsFailed, [("operation", .string("remove")), ("path", .string(path)), ("reason", .string("the folder isn't empty"))],
                             severity: .error).toError()
        }
    }

    func moveToTrash(_ path: String, trash: String) throws(BroError) { fatalError("not used") }
    func syncFile(_ path: String) throws(BroError) { fatalError("not used") }

    func syncDirectory(_ path: String) throws(BroError) {
        try before("syncDirectory", path)
        guard lock.withLock({ dirs.contains(path) }) else { throw fsError(.fsNotFound, path) }
        _ = lock.withLock { synced.insert(path) }
    }

    private final class Stream: ByteStream, @unchecked Sendable {
        let bytes: [UInt8]
        var at = 0
        init(_ bytes: [UInt8]) { self.bytes = bytes }

        func read(_ maxBytes: Int) throws(BroError) -> [UInt8] {
            let n = min(maxBytes, 4, bytes.count - at)
            defer { at += n }
            return Array(bytes[at..<at + n])
        }

        func close() {}
    }

    func openForReading(_ path: String, bypassCache: Bool) throws(BroError) -> any ByteStream { Stream(try read(path)) }

    func volume(_ path: String) throws(BroError) -> VolumeInfo {
        try before("volume", path)
        return VolumeInfo(id: "volume", freeBytes: free, fsType: "apfs", caseSensitive: true)
    }
}

/// The libraries table and the unit tables in memory; a unit's library must be stored (the foreign key).
private final class Db: LibraryRepository, UnitRepository, @unchecked Sendable {
    private let lock = NSLock()
    var libraries: [String: LibraryRecord] = [:]
    var units: [String: UnitRecord] = [:]
    var intents: [Id: CommitIntent] = [:]
    var files: [String: [UnitFile]] = [:]

    func all() async throws(BroError) -> [LibraryRecord] { lock.withLock { libraries.values.sorted { ordinal($0.id, $1.id) } } }
    func get(_ libraryId: String) async throws(BroError) -> LibraryRecord? { lock.withLock { libraries[libraryId] } }
    func upsert(_ library: LibraryRecord) async throws(BroError) { lock.withLock { libraries[library.id] = library } }

    func setSeen(_ libraryId: String, online: Bool, at: Instant) async throws(BroError) {
        lock.withLock {
            guard var l = libraries[libraryId] else { return }
            l.online = online
            if online { l.lastSeenAt = at }
            libraries[libraryId] = l
        }
    }

    func beginCommit(_ unit: UnitRecord, intent: CommitIntent) async throws(BroError) {
        let stored = lock.withLock { () -> Bool in
            guard libraries[unit.libraryId] != nil else { return false }
            units[unit.id.value] = unit
            intents[unit.id] = intent
            return true
        }
        if !stored { throw BroError("store.failed", [("reason", .string("FOREIGN KEY"))]) }
    }

    func markMoved(_ unitId: Id, seq: Int) async throws(BroError) {
        lock.withLock {
            guard var i = intents[unitId] else { return }
            i.items = i.items.map { var x = $0; if x.seq == seq { x.moved = true }; return x }
            intents[unitId] = i
        }
    }

    func finishCommit(_ intent: CommitIntent, record: UnitRecord, files: [UnitFile]) async throws(BroError) {
        lock.withLock {
            units[record.id.value] = record
            self.files[record.id.value] = files
            intents.removeValue(forKey: intent.unitId)
        }
    }

    func openIntents() async throws(BroError) -> [CommitIntent] { open() }

    func open() -> [CommitIntent] {
        lock.withLock { intents.values.sorted { ($0.createdAt.unixMilliseconds, $0.unitId.value) < ($1.createdAt.unixMilliseconds, $1.unitId.value) } }
    }

    func get(_ unitId: Id) async throws(BroError) -> UnitRecord? { lock.withLock { units[unitId.value] } }
    func query(_ query: UnitQuery) async throws(BroError) -> [UnitRecord] { fatalError("not used") }
    func leastRecentlyVerified(_ limit: Int) async throws(BroError) -> [UnitRecord] { fatalError("not used") }
    func markMissing(_ unitId: Id) async throws(BroError) { fatalError("not used") }
}

@EngineActor
private final class LibraryWorld {
    var events: [EngineEvent] = []
    var results: [Int: JsonValue] = [:]
    var bus: EventBus!
    var problems: ProblemService!
    var scheduler: Scheduler!
    var service: LibraryService!
    var subscription: Subscription?
}

private func intentsJson(_ intents: [CommitIntent]) -> JsonValue {
    .array(intents.map { i in
        .array([.string(i.unitId.value), .array(i.items.sorted { $0.seq < $1.seq }.map { x in
            .string("\(x.seq) \(x.fromPath) \(x.toPath)\(x.isDir ? " dir" : "")\(x.moved ? " moved" : "")")
        })])
    })
}

private func resultJson(_ r: CommitResult) -> JsonValue {
    .object([("unitId", .string(r.unitId.value)), ("destination", .string(r.destination)),
             ("files", .array(r.files.map(\.path).sorted(by: ordinal).map { .string($0) })), ("merged", .bool(r.merged))])
}

private func rowJson(_ l: LibraryRecord) -> JsonValue {
    .object([("id", .string(l.id)), ("name", .string(l.name)), ("path", .string(l.path)), ("markerId", .string(l.markerId)),
             ("attachedAt", .string(Instant.format(l.attachedAt))), ("online", .bool(l.online)),
             ("lastSeenAt", l.lastSeenAt.map { .string(Instant.format($0)) } ?? .null)])
}

/// shared/fixtures/engine/library-service.cases.json, on a real executor, scheduler and problem service, with a file
/// system and database in memory.
struct LibraryServiceTests {
    @Test func theSharedCasesPass() async throws {
        let doc = try Fixtures.json("engine/library-service.cases.json")
        var failures: [String] = []
        for c in doc["cases"]!.array! {
            let id = c["id"]!.string!
            let world = World()
            do {
                try await Self.run(c["given"]!, c["expect"]!, world)
            } catch {
                failures.append("\(id): \(error)")
            }
            await world.executor.stop()
        }
        #expect(failures.isEmpty, "\(failures.joined(separator: "\n"))")
    }

    @Test func sha256HasherHashesInPieces() {
        let h = Sha256Hasher()
        for piece in ["a", "", "bc"] { h.update(Array(piece.utf8)) }
        #expect(h.finish() == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")
    }

    private static func run(_ given: JsonValue, _ expect: JsonValue, _ world: World) async throws {
        let tree = Tree()
        for d in given["dirs"]?.array ?? [] { tree.makeDirs(d.string!) }
        for (p, t) in given["files"]?.members ?? [] { tree.put(p, t.string!) }
        for f in given["fail"]?.array ?? [] { tree.fails.append((f["op"]!.string!, f["path"]!.string!, f["error"]!, Int(f["times"]?.int ?? -1))) }
        for h in given["hold"]?.array ?? [] { tree.holds.append((h["op"]!.string!, h["path"]!.string!)) }
        let db = Db()
        for l in given["libraries"]?.array ?? [] {
            db.libraries[l["id"]!.string!] = LibraryRecord(id: l["id"]!.string!, name: l["name"]!.string!, path: l["path"]!.string!,
                                                           markerId: l["markerId"]!.string!, attachedAt: Instant.parse(l["attachedAt"]!.string!)!,
                                                           online: l["online"]!.bool!, lastSeenAt: l["lastSeenAt"]?.string.flatMap(Instant.parse))
        }
        let config = await CaseConfig()
        let libraries = given["config"] ?? .array([])
        await world.onExecutor { config.document = .object([("version", .integer(3)), ("libraries", libraries)]) }
        let crashAt = given["crashAt"]?.string
        let crashed = NSLock()
        nonisolated(unsafe) var didCrash = false
        let crash: @Sendable (String) throws(BroError) -> Void = { point throws(BroError) in
            let now = crashed.withLock { () -> Bool in
                guard point == crashAt, !didCrash else { return false }
                didCrash = true
                return true
            }
            if now { throw BroError("crash", [("point", .string(point))]) }
        }

        let state = await LibraryWorld()
        @Sendable func build() async {
            await world.onExecutor {
                state.bus = EventBus(clock: world.clock)
                state.problems = ProblemService(bus: state.bus, clock: world.clock)
                state.scheduler = Scheduler(executor: world.executor, bus: state.bus, limits: SchedulerLimits())
                state.service = LibraryService(executor: world.executor, bus: state.bus, problems: state.problems, scheduler: state.scheduler,
                                               config: config, clock: world.engineClock, fs: tree, libraries: db, units: db, crash: crash)
                state.subscription = state.bus.subscribe(types: [.libraryChanged, .problemRaised, .problemCleared, .configChanged]) {
                    state.events.append($0)
                }
            }
        }
        await build()

        @Sendable func settled() async -> Bool {
            await withCheckedContinuation { (done: CheckedContinuation<Bool, Never>) in
                // Posted alone (no barrier after it), so the executor's activity counts only the engine's own work.
                if !world.executor.post({ done.resume(returning: world.executor.activity == tree.held) }) { done.resume(returning: true) }
            }
        }
        @Sendable func settle() async throws {
            let until = Date().addingTimeInterval(10)
            while !(await settled()) {
                if Date() > until { throw FixtureError("the service didn't settle") }
                try await Task.sleep(nanoseconds: 1_000_000)
            }
        }
        @Sendable func store(_ i: Int, _ r: JsonValue) async { await world.onExecutor { state.results[i] = r } }
        @Sendable func outcome(_ body: @Sendable () async throws(BroError) -> JsonValue) async -> JsonValue {
            do throws(BroError) { return try await body() } catch { return errorJson(error) }
        }

        let ops = given["ops"]!.array!
        var pending: [Task<Void, Never>] = []
        func onExecutor(_ i: Int, _ arg: JsonValue, _ body: @escaping @EngineActor @Sendable () async throws(BroError) -> JsonValue) async throws {
            let task = Task { @EngineActor in await store(i, await outcome { () async throws(BroError) in try await body() }) }
            if arg["wait"]?.bool == false { pending.append(task) } else { await task.value }
            try await settle()
        }
        func offExecutor(_ i: Int, _ arg: JsonValue, _ body: @escaping @Sendable () async throws(BroError) -> JsonValue) async throws {
            let task = Task.detached { await store(i, await outcome { () async throws(BroError) in try await body() }) }
            if arg["wait"]?.bool == false {
                pending.append(task)
                let until = Date().addingTimeInterval(10)
                while tree.held == 0 {
                    if Date() > until { throw FixtureError("the op never reached its held call") }
                    try await Task.sleep(nanoseconds: 1_000_000)
                }
            } else {
                await task.value
            }
            try await settle()
        }

        try await settle()
        for (i, op) in ops.enumerated() {
            let (name, arg): (String, JsonValue) = op.string.map { ($0, .object([])) } ?? (op.members![0].key, op.members![0].value)
            switch name {
            case "start":
                try await onExecutor(i, arg) { () async throws(BroError) in try await state.service.start(); return .string("ok") }
            case "check":
                try await onExecutor(i, arg) { () async throws(BroError) in await state.service.check(); return .string("ok") }
            case "libraries":
                await world.onExecutor { state.results[i] = .array(state.service.libraries().map(LibraryDto.json)) }
            case "require":
                let service = await state.service!
                await store(i, await outcome { () async throws(BroError) in
                    let h = try service.require(arg.string!)
                    return .object([("id", .string(h.id)), ("path", .string(h.path))])
                })
            case "staging":
                let service = await state.service!
                await store(i, await outcome { () async throws(BroError) in
                    .string(try service.stagingDirectory(arg["library"]!.string!, jobId: Id(arg["job"]!.string!)))
                })
            case "blocked":
                let probe = Id("probe-\(i)")
                let lib = arg.string!
                final class Probe: @unchecked Sendable {
                    var code: String?
                    var granted: Lease?
                    var task: Task<Void, Never>?
                }
                let p = Probe()
                await world.onExecutor {
                    state.scheduler.submit(probe, queue: .processing)
                    p.task = Task { @EngineActor in
                        let lease = try? await state.scheduler.acquire(probe, resources: ResourceSet(drives: [], acquisition: false, libraryWrites: [lib], cpu: 0,
                                                                                                      io: []),
                                                                       onWait: { if case let .blocked(reason) = $0 { p.code = reason.code } },
                                                                       cancel: CancellationSource().token)
                        p.granted = lease
                    }
                }
                try await settle()
                await world.onExecutor {
                    if let lease = p.granted { state.scheduler.release(lease) }
                    state.scheduler.finish(probe)
                }
                await p.task?.value
                try await settle()
                await store(i, p.granted != nil ? .null : p.code.map { .string($0) } ?? .null)
            case "attach":
                try await onExecutor(i, arg) { () async throws(BroError) in
                    LibraryDto.json(try await state.service.attach(arg["id"]!.string!, path: arg["path"]!.string!, name: arg["name"]!.string!))
                }
            case "configure":
                await world.onExecutor {
                    config.document = .object([("version", .integer(3)), ("libraries", arg)])
                    state.bus.publish(.configChanged, data: .object([("version", .integer(1))]))
                }
                try await settle()
                await store(i, .string("ok"))
            case "advance":
                await world.clock.advance(Int64((arg.double! * 1000).rounded())) { try? await settle() }
                try await settle()
                await store(i, .string("ok"))
            case "fs":
                for (p, t) in arg["put"]?.members ?? [] { tree.put(p, t.string!) }
                if let away = arg["moveAway"]?.string { tree.moveAway(away) }
                await store(i, .string("ok"))
            case "held":
                try await settle()
                await store(i, .integer(Int64(tree.held)))
            case "release":
                tree.release()
                // A woken call counts as held until it leaves the gate: wait for that, or the settle check passes early.
                let until = Date().addingTimeInterval(10)
                while tree.held > 0 {
                    if Date() > until { throw FixtureError("the held calls didn't leave") }
                    try await Task.sleep(nanoseconds: 1_000_000)
                }
                for t in pending { await t.value }
                try await settle()
                await store(i, .string("ok"))
            case "commit", "quarantine":
                let u = arg["unit"]!, it = arg["intent"]!, rows = arg["files"]!.array!
                let unit = UnitRecord(id: Id(u["id"]!.string!), libraryId: "lib", jobId: Id("00000000-0000-4000-8000-000000000001"), path: u["path"]!.string!,
                                      recordFile: "bromelia-00000000.json", recordVersion: 3, state: .committing, status: .success, name: u["name"]!.string!,
                                      kind: .movie, format: "bluray", formatCode: "BD", encrypted: false, label: "", makemkvVersion: "", bytes: 0,
                                      fileCount: rows.count, attempts: 1, createdAt: world.clock.now())
                let items = it["items"]!.array!.enumerated().map { n, x in
                    CommitItem(seq: n, fromPath: x.array![0].string!, toPath: x.array![1].string!, sha256: x.array![2].string, isDir: x.array![3].bool!,
                               moved: false)
                }
                let intent = CommitIntent(unitId: unit.id, jobId: unit.jobId!, staging: it["staging"]!.string!, destination: it["destination"]!.string!,
                                          merge: it["merge"]!.bool!, quarantine: false, createdAt: world.clock.now(), items: items)
                let files = rows.map { f in
                    UnitFile(path: f.array![0].string!, size: f.array![1].int!, sha256: f.array![2].string!, role: f.array![3].string!)
                }
                let tag = arg["tag"]?.string
                let service = await state.service!
                try await offExecutor(i, arg) { () async throws(BroError) in
                    if let tag { return resultJson(try await service.quarantine(unit, intent: intent, files: files, tag: tag)) }
                    return resultJson(try await service.commit(unit, intent: intent, files: files))
                }
            case "restart":
                await world.onExecutor { state.service.stop() }
                try await settle()
                await build()
                await store(i, .string("ok"))
            case "rollForward":
                let service = await state.service!
                try await offExecutor(i, arg) { () async throws(BroError) in
                    var list: [JsonValue] = []
                    for intent in db.open() {
                        do throws(BroError) { list.append(resultJson(try await service.rollForward(intent))) } catch { list.append(errorJson(error)) }
                    }
                    return .array(list)
                }
            case "dump":
                await store(i, intentsJson(db.open()))
            default:
                throw FixtureError("unknown op \(name)")
            }
        }
        for t in pending { await t.value }
        try await settle()

        let results = await state.results
        try Fixtures.same(expect["results"]!, .array((0..<ops.count).map { results[$0] ?? .null }), "results")
        let (events, problems) = await (state.events, state.problems.problems())
        if let want = expect["events"] {
            let got = JsonValue.array(events.map(EngineEvent.json))
            try Fixtures.same(Fixtures.sorted(want), Fixtures.sorted(got), "events")
        }
        if let want = expect["problems"] { try Fixtures.same(Fixtures.sorted(want), Fixtures.sorted(.array(problems.map(ProblemDto.json))), "problems") }
        if let want = expect["files"] {
            try Fixtures.same(Fixtures.sorted(want), Fixtures.sorted(.object(tree.files.map { ($0.key, .string(String(decoding: $0.value, as: UTF8.self))) })),
                              "files")
        }
        for (p, mode) in expect["modes"]?.members ?? [] { try Fixtures.same(mode.int, tree.modes[p].map(Int64.init), "mode of \(p)") }
        if let want = expect["dirs"] { try Fixtures.same(want, .array(tree.dirs.sorted(by: ordinal).map { .string($0) }), "dirs") }
        if let want = expect["synced"] { try Fixtures.same(want, .array(tree.synced.sorted(by: ordinal).map { .string($0) }), "synced") }
        if let want = expect["db"] {
            if let libs = want["libraries"] {
                try Fixtures.same(Fixtures.sorted(libs), Fixtures.sorted(.array(db.libraries.values.sorted { ordinal($0.id, $1.id) }.map(rowJson))), "db libraries")
            }
            if let units = want["units"] {
                let got = JsonValue.array(db.units.values.sorted { ordinal($0.id.value, $1.id.value) }.map { u in
                    var m: [(key: String, value: JsonValue)] = [("id", .string(u.id.value)), ("path", .string(u.path)), ("state", .string(u.state.rawValue))]
                    if let c = u.committedAt { m.append(("committedAt", .string(Instant.format(c)))) }
                    return .object(m)
                })
                try Fixtures.same(Fixtures.sorted(units), Fixtures.sorted(got), "db units")
            }
            if let intents = want["intents"] { try Fixtures.same(intents, intentsJson(db.open()), "db intents") }
            if let uf = want["unitFiles"] {
                let got = JsonValue.object(db.files.map { key, files in
                    (key, .array(files.map { .array([.string($0.path), .integer($0.size), .string($0.sha256), .string($0.role)]) }))
                })
                try Fixtures.same(Fixtures.sorted(uf), Fixtures.sorted(got), "db unit files")
            }
        }
    }
}
