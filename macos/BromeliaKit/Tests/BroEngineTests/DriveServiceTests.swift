import BroDomain
import BroEngine
import BroFoundation
import BroPorts
import BroTestSupport
import Foundation
import Testing

private func driveFailure(_ j: JsonValue) -> BroError { BroError(j["code"]!.string!, j["params"]?.members ?? []) }

/// The ports DriveService uses, in memory; calls are recorded in order.
private final class DriveFakes: Makemkv, DeviceMonitor, DriveControl, DriveRepository, ConfigSource, @unchecked Sendable {
    private let lock = NSLock()
    private var recorded: [String] = []
    var makemkv: [JsonValue] = []
    var os: [OsDrive] = []
    var scanFails: JsonValue?
    var ejectFails: JsonValue?
    var sink: (@Sendable (DeviceEvent) -> Void)?
    var stored: [String: DriveRecord] = [:]
    var config = try! ConfigCodec.decode(Array("{\"version\": 3}".utf8))

    var calls: [String] { lock.withLock { recorded } }
    func call(_ c: String) { lock.withLock { recorded.append(c) } }
    func with<T>(_ body: (DriveFakes) -> T) -> T { lock.withLock { body(self) } }

    func scanDrives(_ cancel: CancellationToken) async throws(BroError) -> [MakemkvDrive] {
        call("scan")
        let (fails, lines) = with { ($0.scanFails, $0.makemkv) }
        if let fails { throw driveFailure(fails) }
        return lines.compactMap { Robot.parseLine($0.string!).flatMap(MakemkvDrive.from) }
    }
    func listing(_ source: MakemkvSource, invocation: MakemkvInvocation, sink: any RunSink, cancel: CancellationToken) async throws(BroError) -> ListingRun {
        throw BroError("test.unsupported")
    }
    func rip(_ source: MakemkvSource, title: String, destination: String, invocation: MakemkvInvocation, sink: any RunSink,
             cancel: CancellationToken) async throws(BroError) -> MakemkvRun { throw BroError("test.unsupported") }
    func backup(_ source: MakemkvSource, decrypt: Bool, destination: String, invocation: MakemkvInvocation, sink: any RunSink,
                cancel: CancellationToken) async throws(BroError) -> MakemkvRun { throw BroError("test.unsupported") }

    func start(_ sink: @escaping @Sendable (DeviceEvent) -> Void) {
        call("monitor.start")
        with { $0.sink = sink }
    }
    func stop() {}
    func currentDrives() -> [OsDrive] { with { $0.os } }

    func eject(_ device: String) async throws(BroError) {
        call("eject " + device)
        if let f = with({ $0.ejectFails }) { throw driveFailure(f) }
    }
    func closeTray(_ device: String) async throws(BroError) { call("closeTray " + device) }
    func waitForMount(_ device: String, timeout: Duration, cancel: CancellationToken) async -> String? { nil }
    func probeContent(_ device: String) -> DiscContent {
        call("probe " + device)
        return .video
    }
    func openRaw(_ device: String) throws(BroError) -> any SectorReader { throw BroError("test.unsupported") }

    func upsert(_ drive: DriveRecord) async throws(BroError) { with { $0.stored[drive.id] = drive } }
    func all() async throws(BroError) -> [DriveRecord] { with { Array($0.stored.values) } }
    func recordStats(_ driveId: String, day: String, stats: DriveStats) async throws(BroError) { throw BroError("test.unsupported") }

    @EngineActor func current() -> Config { with { $0.config } }
}

@EngineActor
private final class DriveEvents {
    var events: [JsonValue] = []
    var subscription: Subscription?
}

/// shared/fixtures/engine/drive-service.cases.json, on a real executor, bus, problem service and engine clock with fake
/// ports.
@Suite struct DriveServiceTests {
    private static func osOf(_ o: JsonValue) -> OsDrive {
        OsDrive(device: o["device"]!.string!, identification: o["identification"]?.string ?? "", mountPath: o["mountPath"]?.string)
    }

    private static func rowOf(_ r: JsonValue) -> DriveRecord {
        DriveRecord(id: r["id"]!.string!, identification: r["identification"]!.string!, model: r["model"]!.string!, lastDevice: r["lastDevice"]!.string!,
                    configId: r["configId"]?.string, firstSeenAt: Instant.parse(r["firstSeenAt"]!.string!)!,
                    lastSeenAt: Instant.parse(r["lastSeenAt"]!.string!)!)
    }

    private static func rowJson(_ r: DriveRecord) -> JsonValue {
        .object([("id", .string(r.id)), ("identification", .string(r.identification)), ("model", .string(r.model)), ("lastDevice", .string(r.lastDevice)),
                 ("configId", r.configId.map { .string($0) } ?? .null), ("firstSeenAt", .string(Instant.format(r.firstSeenAt))),
                 ("lastSeenAt", .string(Instant.format(r.lastSeenAt)))])
    }

    private static func locationJson(_ l: DriveLocation?) -> JsonValue {
        guard let l else { return .null }
        return .object([("makemkvIndex", l.makemkvIndex.map { .integer(Int64($0)) } ?? .null), ("device", .string(l.device)),
                        ("configId", l.configId.map { .string($0) } ?? .null), ("generation", l.generation.map { .integer($0) } ?? .null),
                        ("label", .string(l.label)), ("flags", l.flags.map { .integer(Int64($0.raw)) } ?? .null)])
    }

    private static func eventOf(_ e: JsonValue) throws -> DeviceEvent {
        let device = e["device"]!.string!
        switch e["type"]!.string! {
        case "driveAppeared": return .driveAppeared(drive: OsDrive(device: device))
        case "driveVanished": return .driveVanished(device: device)
        case "mediaArrived": return .mediaArrived(device: device)
        case "mediaRemoved": return .mediaRemoved(device: device)
        case "trayOpened": return .trayOpened(device: device)
        case "mounted": return .mounted(device: device, path: e["path"]!.string!)
        case "unmounted": return .unmounted(device: device)
        case let t: throw FixtureError("unknown device event \(t)")
        }
    }

    private static func same(_ want: JsonValue, _ got: JsonValue, _ what: String) throws {
        let w = String(decoding: JsonValue.encodeCanonical(want), as: UTF8.self)
        let g = String(decoding: JsonValue.encodeCanonical(got), as: UTF8.self)
        if w != g { throw FixtureError("\(what): expected\n\(w)got\n\(g)") }
    }

    @Test func theSharedCasesPass() async throws {
        let doc = try Fixtures.json("engine/drive-service.cases.json")
        var failures: [String] = []
        for c in doc["cases"]!.array! {
            let id = c["id"]!.string!, given = c["given"]!, expect = c["expect"]!
            let world = World()
            do {
                try await runCase(given, expect, world)
            } catch {
                failures.append("\(id): \(error)")
            }
            await world.executor.stop()
        }
        #expect(failures.isEmpty, "\(failures.joined(separator: "\n"))")
    }

    private func runCase(_ given: JsonValue, _ expect: JsonValue, _ world: World) async throws {
        let bus = EventBus(clock: world.clock)
        let problems = await ProblemService(bus: bus, clock: world.clock)
        let f = DriveFakes()
        f.makemkv = given["makemkv"]?.array ?? []
        f.os = (given["os"]?.array ?? []).map(Self.osOf)
        f.ejectFails = given["ejectFails"]
        if let doc = given["config"] { f.config = try ConfigCodec.decode(JsonValue.encodeCanonical(doc)) }
        for r in given["stored"]?.array ?? [] { f.stored[r["id"]!.string!] = Self.rowOf(r) }
        let service = await DriveService(executor: world.executor, bus: bus, problems: problems, config: f, clock: world.engineClock, monitor: f,
                                         control: f, makemkv: f, repository: f)
        let recorder = await DriveEvents()
        await world.onExecutor {
            recorder.subscription = bus.subscribe(types: [.driveChanged, .driveRemoved, .mediumArrived, .mediumRemoved, .problemRaised, .problemCleared]) {
                recorder.events.append(.object([("seq", .integer($0.seq)), ("at", .string(Instant.format($0.at))), ("type", .string($0.type.rawValue)),
                                                ("data", $0.data)]))
            }
        }

        @Sendable func settled() async -> Bool {
            await withCheckedContinuation { (done: CheckedContinuation<Bool, Never>) in
                // Posted alone (no barrier after it), so the executor's activity counts only the engine's own work.
                if !world.executor.post({ done.resume(returning: world.executor.activity == 0) }) { done.resume(returning: true) }
            }
        }
        @Sendable func settle() async throws {
            let until = Date().addingTimeInterval(10)
            while !(await settled()) {
                if Date() > until { throw FixtureError("the service didn't settle") }
                try await Task.sleep(nanoseconds: 1_000_000)
            }
        }
        /// Runs a command on the executor and waits for it; its failure is the result.
        func run(_ command: @escaping @EngineActor @Sendable () async throws(BroError) -> Void) async throws -> JsonValue {
            let result: JsonValue = await Task { @EngineActor in
                do throws(BroError) {
                    try await command()
                    return .null
                } catch {
                    return .object([("error", .object([("code", .string(error.code)), ("params", .object(error.params))]))])
                }
            }.value
            try await settle()
            return result
        }

        var results: [JsonValue] = []
        for op in given["ops"]!.array! {
            let name = op.string ?? op.members![0].key
            let arg = op.string != nil ? JsonValue.null : op.members![0].value
            var result = JsonValue.null
            switch name {
            case "start": result = try await run { () async throws(BroError) in try await service.start() }
            case "set":
                if let mk = arg["makemkv"]?.array { f.with { $0.makemkv = mk } }
                if let os = arg["os"]?.array { f.with { $0.os = os.map(Self.osOf) } }
                if arg.members!.contains(where: { $0.key == "scanFails" }) {
                    let fails = arg["scanFails"].flatMap { $0.isNull ? nil : $0 }
                    f.with { $0.scanFails = fails }
                }
            case "device":
                let event = try Self.eventOf(arg)
                f.with { $0.sink }!(event)
                try await settle()
            case "advance":
                await world.clock.advance(arg.int! * 1000) { try? await settle() }
                try await settle()
            case "lease":
                let json: JsonValue = .object([("drives", arg["drives"]!), ("acquisitions", arg["acquisitions"]!),
                                               ("jobs", .object([("acquisition", .integer(0)), ("processing", .integer(0)), ("maintenance", .integer(0))]))])
                await world.onExecutor { _ = bus.publish(.leaseChanged, data: json) }
                try await settle()
            case "config":
                let config = try ConfigCodec.decode(JsonValue.encodeCanonical(arg))
                await world.onExecutor {
                    f.with { $0.config = config }
                    _ = bus.publish(.configChanged, data: .object([("version", .integer(2))]))
                }
                try await settle()
            case "rescan": result = try await run { await service.rescan() }
            case "eject": result = try await run { () async throws(BroError) in try await service.eject(arg.string!) }
            case "closeTray": result = try await run { () async throws(BroError) in try await service.closeTray(arg.string!) }
            case "closeAll": result = try await run { await service.closeAll() }
            case "drives": result = .array(await Task { @EngineActor in service.drives().map(DriveDto.json) }.value)
            case "find": result = Self.locationJson(service.find(arg.string!))
            default: throw FixtureError("unknown op \(name)")
            }
            results.append(result)
        }
        await world.onExecutor { service.stop() }
        try await settle()

        if let want = expect["results"] { try Self.same(want, .array(results), "results") }
        if let want = expect["events"] { try Self.same(want, .array(await Task { @EngineActor in recorder.events }.value), "events") }
        if let want = expect["calls"] { try Self.same(want, .array(f.calls.map { .string($0) }), "calls") }
        if let want = expect["problems"] {
            try Self.same(want, .array(await Task { @EngineActor in problems.problems().map(ProblemDto.json) }.value), "problems")
        }
        if let want = expect["stored"] {
            try Self.same(want, .array(f.with { $0.stored.values.sorted { $0.id < $1.id } }.map(Self.rowJson)), "stored")
        }
    }
}
