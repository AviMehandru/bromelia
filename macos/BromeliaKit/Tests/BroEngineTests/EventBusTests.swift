import BroEngine
import BroFoundation
import BroTestSupport
import Testing

/// shared/fixtures/engine/event-bus.cases.json.
@EngineActor
struct EventBusTests {
    static func line(_ e: EngineEvent) -> String { "\(e.seq) \(e.type.rawValue)" }

    static func types(_ j: JsonValue?) throws -> [EngineEventType] {
        try (j?.array ?? []).map { t in
            guard let type = EngineEventType(rawValue: t.string ?? "") else { throw FixtureError("unknown type \(t)") }
            return type
        }
    }

    @EngineActor
    final class Harness {
        let clock = ManualClock()
        let bus: EventBus
        var subs: [String: Subscription] = [:]
        var received: [String: [String]] = [:]
        var streams: [String: EventStream] = [:]
        var read: [String: [String]] = [:]
        var published: [EngineEvent] = []

        init(_ given: JsonValue) {
            bus = EventBus(clock: clock, firstSeq: given["firstSeq"]?.int ?? 1, retain: Int(given["retain"]?.int ?? 1024),
                           streamCapacity: Int(given["streamCapacity"]?.int ?? 1024))
        }

        func run(_ ops: [JsonValue]) throws {
            for op in ops {
                guard let (name, arg) = op.members?.first else { throw FixtureError("bad op \(op)") }
                switch name {
                case "publish":
                    guard let type = EngineEventType(rawValue: arg["type"]?.string ?? "") else { throw FixtureError("bad type") }
                    published.append(bus.publish(type, data: arg["data"] ?? .object([])))
                case "advance":
                    clock.advanceNow(arg.int!)
                case "subscribe":
                    let name = arg["as"]!.string!
                    received[name] = []
                    let on = arg["onReceive"]
                    subs[name] = bus.subscribe(types: try EventBusTests.types(arg["types"])) { [unowned self] e in
                        self.received[name, default: []].append(EventBusTests.line(e))
                        if let on, on["type"]?.string == e.type.rawValue { try? self.run(on["do"]!.array!) }
                    }
                case "unsubscribe":
                    subs[arg.string!]!.cancel()
                case "stream":
                    let name = arg["as"]!.string!
                    streams[name] = bus.stream(after: arg["after"]?.int, types: try EventBusTests.types(arg["types"]))
                    read[name] = []
                case "read":
                    let s = streams[arg.string!]!
                    while let item = s.poll() {
                        switch item {
                        case let .event(e): read[arg.string!]!.append(EventBusTests.line(e))
                        case .resync: read[arg.string!]!.append("resync")
                        }
                    }
                    if s.isClosed { read[arg.string!]!.append("end") }
                case "close":
                    streams[arg.string!]!.close()
                default:
                    throw FixtureError("unknown op \(name)")
                }
            }
        }
    }

    static func lines(_ j: JsonValue) -> [String] { j.array!.map { $0.string! } }

    @Test func theSharedCasesPass() throws {
        let failures = try Fixtures.runCases("engine/event-bus.cases.json") { id, given, expect in
            if id == "event-types" {
                let all = EngineEventType.allCases
                try Fixtures.same(EventBusTests.lines(expect["clientVisible"]!), all.filter { !$0.isInternal }.map(\.rawValue), "client-visible")
                try Fixtures.same(EventBusTests.lines(expect["internal"]!), all.filter(\.isInternal).map(\.rawValue), "internal")
                let schema = try Fixtures.sharedJson(expect["asInSchema"]!.string!)
                let consts = schema["oneOf"]!.array!.map { ref -> String in
                    let def = String(ref["$ref"]!.string!.split(separator: "/").last!)
                    return schema["$defs"]![def]!["properties"]!["type"]!["const"]!.string!
                }
                try Fixtures.same(consts, all.filter { !$0.isInternal }.map(\.rawValue), "events.json")
                return true
            }
            let h = Harness(given)
            try h.run(given["ops"]!.array!)
            if let last = expect["lastSeq"]?.int { try Fixtures.same(last, h.bus.lastSeq, "lastSeq") }
            if let events = expect["events"] { try Fixtures.same(events, .array(h.published.map(EngineEvent.json)), "events") }
            for (name, want) in expect["received"]?.members ?? [] {
                try Fixtures.same(EventBusTests.lines(want), h.received[name] ?? [], "received by \(name)")
            }
            for (name, want) in expect["read"]?.members ?? [] {
                try Fixtures.same(EventBusTests.lines(want), h.read[name] ?? [], "read from \(name)")
            }
            return true
        }
        #expect(failures.isEmpty, "\(failures.joined(separator: "\n"))")
    }

    @Test func nextWaitsForAnEventAndEndsWhenClosed() async throws {
        let bus = EventBus(clock: ManualClock())
        let stream = bus.stream(after: nil, types: [])
        let next = Task.detached { await stream.next() }
        try await Task.sleep(nanoseconds: 20_000_000)
        bus.publish(.jobChanged, data: .object([]))
        let item = await next.value
        #expect(item == .event(EngineEvent(seq: 1, at: Instant(unixMilliseconds: ManualClock.startMs), type: .jobChanged, data: .object([]))))
        let end = Task.detached { await stream.next() }
        try await Task.sleep(nanoseconds: 20_000_000)
        stream.close()
        #expect(await end.value == nil)
    }
}
