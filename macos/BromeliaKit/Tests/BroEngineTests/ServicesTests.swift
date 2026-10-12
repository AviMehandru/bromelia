import BroDomain
import BroEngine
import BroFoundation
import BroPorts
import BroTestSupport
import Foundation
import Testing

private let configPath = "/data/config.json"

private func failure(_ j: JsonValue) -> BroError { BroError(j["code"]!.string!, j["params"]?.members ?? []) }

private func errorJson(_ e: BroError) -> JsonValue { .object([("error", .object([("code", .string(e.code)), ("params", .object(e.params))]))]) }

/// A document as ConfigCodec.decode leaves it, so a case can write it short.
private func norm(_ doc: JsonValue) -> JsonValue { (try? ConfigCodec.decode(JsonValue.encodeCanonical(doc)))?.document ?? doc }

private func replacing(_ obj: JsonValue, _ key: String, _ value: JsonValue) -> JsonValue {
    .object((obj.members ?? []).map { $0.key == key ? (key, value) : $0 })
}

private func scope(_ j: JsonValue?) -> ProblemScope? {
    guard let j else { return nil }
    return ProblemScope(kind: ProblemScopeKind(rawValue: j["kind"]!.string!)!, id: j["id"]!.string!)
}

private final class ConfigFiles: FileSystem, @unchecked Sendable {
    private let lock = NSLock()
    private var bytes: [UInt8]?
    private var writtenMode: Int?
    var writeFails: JsonValue?
    var hold: DispatchSemaphore?
    private var isHeld = false

    var content: [UInt8]? { lock.withLock { bytes } }
    var mode: Int? { lock.withLock { writtenMode } }
    var held: Bool { lock.withLock { isHeld } }

    init(_ initial: [UInt8]?) { bytes = initial }

    func exists(_ path: String) -> Bool { lock.withLock { path == configPath && bytes != nil } }

    func read(_ path: String) throws(BroError) -> [UInt8] {
        guard let b = lock.withLock({ bytes }) else { throw BroError("fs.notFound", [("path", .string(path))]) }
        return b
    }

    /// With holdWrites, every write (kv or file) waits here for releaseWrites.
    func waitIfHeld() throws(BroError) {
        guard let hold else { return }
        lock.withLock { isHeld = true }
        if hold.wait(timeout: .now() + 10) == .timedOut { throw BroError("test.neverReleased") }
        hold.signal() // released for good: let every later write through
    }

    func writeAtomically(_ path: String, bytes: [UInt8], mode: Int) throws(BroError) {
        try waitIfHeld()
        if let writeFails { throw failure(writeFails) }
        lock.withLock {
            self.bytes = bytes
            writtenMode = mode
        }
    }

    func stat(_ path: String) throws(BroError) -> FileInfo { throw BroError("test.unsupported") }
    func list(_ directory: String) throws(BroError) -> [DirectoryEntry] { throw BroError("test.unsupported") }
    func readRange(_ path: String, offset: Int64, length: Int) throws(BroError) -> [UInt8] { throw BroError("test.unsupported") }
    func createDirectory(_ path: String, parentsMustExist: Bool) throws(BroError) { throw BroError("test.unsupported") }
    func rename(_ from: String, to: String) throws(BroError) { throw BroError("test.unsupported") }
    func moveMerging(_ from: String, to: String, policy: MovePolicy, onMoved: (MovedItem) -> Void) throws(BroError) -> [MovedItem] {
        throw BroError("test.unsupported")
    }
    func remove(_ path: String) throws(BroError) { throw BroError("test.unsupported") }
    func moveToTrash(_ path: String, trash: String) throws(BroError) { throw BroError("test.unsupported") }
    func syncFile(_ path: String) throws(BroError) { throw BroError("test.unsupported") }
    func syncDirectory(_ path: String) throws(BroError) { throw BroError("test.unsupported") }
    func openForReading(_ path: String, bypassCache: Bool) throws(BroError) -> any ByteStream { throw BroError("test.unsupported") }
    func volume(_ path: String) throws(BroError) -> VolumeInfo { throw BroError("test.unsupported") }
}

private final class FakeKeystore: Keystore, @unchecked Sendable {
    private let lock = NSLock()
    private var stored: [String: String] = [:]
    var fails: [String: JsonValue] = [:]
    let systemStore: Bool

    var values: [String: String] { lock.withLock { stored } }

    init(_ initial: [String: String], systemStore: Bool) {
        stored = initial
        self.systemStore = systemStore
    }

    func isSystemStore() -> Bool { systemStore }

    /// A change made outside Bromelia.
    func change(_ name: String, to value: String?) { lock.withLock { stored[name] = value } }

    private func check(_ name: String) throws(BroError) { if let f = fails[name] { throw failure(f) } }

    func get(_ name: String) throws(BroError) -> String? {
        try check(name)
        return lock.withLock { stored[name] }
    }

    func set(_ name: String, value: String) throws(BroError) {
        try check(name)
        lock.withLock { stored[name] = value }
    }

    func remove(_ name: String) throws(BroError) {
        try check(name)
        _ = lock.withLock { stored.removeValue(forKey: name) }
    }
}

private final class FakeKv: KeyValueRepository, @unchecked Sendable {
    private let lock = NSLock()
    private var stored: [String: JsonValue] = [:]
    private let gate: ConfigFiles

    var values: [String: JsonValue] { lock.withLock { stored } }

    init(_ initial: [String: JsonValue], gate: ConfigFiles) {
        stored = initial
        self.gate = gate
    }

    func get(_ key: String) async throws(BroError) -> JsonValue? { lock.withLock { stored[key] } }

    func set(_ key: String, value: JsonValue) async throws(BroError) {
        try gate.waitIfHeld()
        lock.withLock { stored[key] = value }
    }
}

@EngineActor
private final class Recorder {
    var events: [EngineEvent] = []
    var results: [Int: JsonValue] = [:]
    var subscription: Subscription?
}

private func eventsAndProblems(_ recorder: Recorder, _ problems: ProblemService) async -> (JsonValue, JsonValue) {
    await (JsonValue.array(recorder.events.map(EngineEvent.json)), JsonValue.array(problems.problems().map(ProblemDto.json)))
}

/// shared/fixtures/engine/problem-service.cases.json and config-service.cases.json, on a real executor.
struct ServicesTests {
    @Test func theSharedProblemServiceCasesPass() async throws {
        let doc = try Fixtures.json("engine/problem-service.cases.json")
        var failures: [String] = []
        for c in doc["cases"]!.array! {
            let id = c["id"]!.string!, given = c["given"]!, expect = c["expect"]!
            let world = World()
            let bus = EventBus(clock: world.clock)
            let problems = await ProblemService(bus: bus, clock: world.clock)
            let recorder = Recorder()
            await world.onExecutor { recorder.subscription = bus.subscribe(types: []) { recorder.events.append($0) } }
            do {
                for op in given["ops"]!.array! {
                    let (name, arg) = op.members![0]
                    switch name {
                    case "raise":
                        let message = BroMessage(MessageCode(rawValue: arg["code"]!.string!)!, arg["params"]?.members ?? [],
                                                 severity: Severity(rawValue: arg["severity"]!.string!)!)
                        let actions = (arg["actions"]?.array ?? []).map { ProblemAction(rawValue: $0.string!)! }
                        await problems.raise(message, scope: scope(arg["scope"]), actions: actions)
                    case "clear":
                        await problems.clear(MessageCode(rawValue: arg["code"]!.string!)!, scope: scope(arg["scope"]))
                    case "advance":
                        await world.advance(arg.int! * 1000)
                    default:
                        throw FixtureError("unknown op \(name)")
                    }
                }
                let (events, list) = await eventsAndProblems(recorder, problems)
                try Fixtures.same(expect["problems"]!, list, "problems")
                try Fixtures.same(expect["events"]!, events, "events")
            } catch {
                failures.append("\(id): \(error)")
            }
            await world.executor.stop()
        }
        #expect(failures.isEmpty, "\(failures.joined(separator: "\n"))")
    }

    @Test func theSharedConfigServiceCasesPass() async throws {
        let doc = try Fixtures.json("engine/config-service.cases.json")
        var failures: [String] = []
        for c in doc["cases"]!.array! {
            let id = c["id"]!.string!, given = c["given"]!, expect = c["expect"]!
            let world = World()
            do {
                try await runConfigCase(given, expect, world)
            } catch {
                failures.append("\(id): \(error)")
            }
            await world.executor.stop()
        }
        #expect(failures.isEmpty, "\(failures.joined(separator: "\n"))")
    }

    private func runConfigCase(_ given: JsonValue, _ expect: JsonValue, _ world: World) async throws {
        var initial: [UInt8]?
        if let file = given["file"] { initial = file["text"]?.string.map { Array($0.utf8) } ?? JsonValue.encodeCanonical(file) }
        let files = ConfigFiles(initial)
        files.writeFails = given["writeFails"]
        if given["holdWrites"]?.bool == true { files.hold = DispatchSemaphore(value: 0) }
        func strings(_ j: JsonValue?) -> [String: String] { Dictionary(uniqueKeysWithValues: (j?.members ?? []).map { ($0.key, $0.value.string!) }) }
        let keystore = FakeKeystore(strings(given["keystore"]), systemStore: true)
        for (k, v) in given["keystoreFails"]?.members ?? [] { keystore.fails[k] = v }
        let secretFiles = FakeKeystore(strings(given["files"]), systemStore: false)
        for (k, v) in given["filesFail"]?.members ?? [] { secretFiles.fails[k] = v }
        // Without a system store, the keystore is the secrets folder.
        let systemStore = given["systemStore"]?.bool != false
        let kv = FakeKv(Dictionary(uniqueKeysWithValues: (given["kv"]?.members ?? []).map { ($0.key, $0.value) }), gate: files)
        let environment = Dictionary(uniqueKeysWithValues: (given["environment"]?.members ?? []).map { ($0.key, $0.value.string!) })
        let bus = EventBus(clock: world.clock)
        let problems = await ProblemService(bus: bus, clock: world.clock)
        let service = await ConfigService(executor: world.executor, bus: bus, problems: problems, fs: files,
                                          keystore: systemStore ? keystore : secretFiles, secretFiles: secretFiles, kv: kv, path: configPath,
                                          environment: environment)
        let recorder = Recorder()
        await world.onExecutor { recorder.subscription = bus.subscribe(types: []) { recorder.events.append($0) } }

        let ops = given["ops"]!.array!
        var pending: [Task<Void, Never>] = []
        @Sendable func result(_ i: Int, _ body: @escaping @EngineActor () async throws -> JsonValue) async {
            let r: JsonValue
            do { r = try await body() } catch let e as BroError { r = errorJson(e) } catch { r = .string("unexpected \(error)") }
            await Self.store(recorder, i, r)
        }
        for (i, op) in ops.enumerated() {
            let (name, arg): (String, JsonValue) = op.string.map { ($0, .null) } ?? (op.members![0].key, op.members![0].value)
            switch name {
            case "load":
                await result(i) { try await service.load(); return .string("ok") }
            case "update":
                let doc = arg["doc"]!, version = arg["version"]!.int!
                if arg["wait"]?.bool == false {
                    pending.append(Task { await result(i) { ConfigEnvelope.json(try await service.update(doc, expectedVersion: version)) } })
                    var waited = 0
                    while !files.held {
                        if waited > 10_000 { throw FixtureError("the update didn't reach its write") }
                        try await Task.sleep(nanoseconds: 1_000_000)
                        waited += 1
                    }
                } else {
                    await result(i) { ConfigEnvelope.json(try await service.update(doc, expectedVersion: version)) }
                }
            case "envelope":
                await result(i) { ConfigEnvelope.json(try await service.envelope()) }
            case "setSecret":
                let n = arg["name"]!.string!, v = arg["value"]!.string!
                await result(i) { try await service.setSecret(n, value: v); return .string("ok") }
            case "validate":
                await result(i) { .array(try service.validate(arg).map { $0.toJson() }) }
            case "current":
                await result(i) { service.current().document }
            case "version":
                await result(i) { .integer(service.version()) }
            case "export":
                await result(i) { JsonValue.parse(service.export())! }
            case "secret":
                // Not on the executor (the keystore may block), with the configuration a job would have.
                let config = await service.current()
                let r: JsonValue
                do throws(BroError) { r = try service.secret(arg.string!, config: config).map { .string($0) } ?? .null } catch { r = errorJson(error) }
                await Self.store(recorder, i, r)
                await world.executor.barrier()
            case "changeOutside":
                (arg["store"]?.string == "keystore" ? keystore : secretFiles).change(arg["name"]!.string!, to: arg["value"]?.string)
            case "releaseWrites":
                files.hold!.signal()
                for t in pending { await t.value }
            default:
                throw FixtureError("unknown op \(name)")
            }
        }
        for t in pending { await t.value }
        await world.executor.barrier()

        let results = await recorder.results
        let wantResults = expect["results"]!.array!
        var r = 0
        for (i, op) in ops.enumerated() where op.string != "releaseWrites" && op["changeOutside"] == nil {
            var want = wantResults[r]
            r += 1
            let name = op.string ?? op.members![0].key
            if want["error"] == nil {
                switch name {
                case "update", "envelope": want = replacing(want, "doc", norm(want["doc"]!))
                case "current": want = norm(want)
                case "export": want = replacing(want, "config", norm(want["config"]!))
                default: break
                }
            }
            try Fixtures.same(Fixtures.sorted(want), Fixtures.sorted(results[i] ?? .null), "result of op \(i) (\(name))")
        }
        try Fixtures.same(wantResults.count, r, "number of results")

        let wantFile = expect["file"]!
        if wantFile.isNull {
            try Fixtures.check(files.content == nil, "file: expected none")
        } else if let text = wantFile["text"]?.string {
            try Fixtures.same(text, String(decoding: files.content ?? [], as: UTF8.self), "file")
        } else {
            try Fixtures.same(Fixtures.sorted(norm(wantFile)), Fixtures.sorted(norm(JsonValue.parse(files.content ?? [])!)), "file")
            if let mode = files.mode {
                try Fixtures.same(0o600, mode, "file mode")
                try Fixtures.same(String(decoding: ConfigCodec.encode(try ConfigCodec.decode(JsonValue.encodeCanonical(wantFile))), as: UTF8.self),
                                  String(decoding: files.content!, as: UTF8.self), "file bytes")
            }
        }
        var wantKv = expect["kv"]!
        if let lastGood = wantKv["config.lastGood"] { wantKv = replacing(wantKv, "config.lastGood", norm(lastGood)) }
        var gotKv = JsonValue.object(kv.values.map { ($0.key, $0.value) })
        if let stored = gotKv["config.lastGood"] { gotKv = replacing(gotKv, "config.lastGood", norm(stored)) }
        try Fixtures.same(Fixtures.sorted(wantKv), Fixtures.sorted(gotKv), "kv")
        try Fixtures.same(Fixtures.sorted(expect["keystore"]!), Fixtures.sorted(.object(keystore.values.map { ($0.key, .string($0.value)) })), "keystore")
        try Fixtures.same(Fixtures.sorted(expect["files"] ?? .object([])), Fixtures.sorted(.object(secretFiles.values.map { ($0.key, .string($0.value)) })),
                          "files")
        let (events, list) = await eventsAndProblems(recorder, problems)
        try Fixtures.same(Fixtures.sorted(expect["events"]!), Fixtures.sorted(events), "events")
        try Fixtures.same(Fixtures.sorted(expect["problems"]!), Fixtures.sorted(list), "problems")
    }

    @EngineActor private static func store(_ recorder: Recorder, _ i: Int, _ r: JsonValue) { recorder.results[i] = r }
}
