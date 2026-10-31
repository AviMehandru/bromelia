import BroAdapters
import BroDomain
import BroEngine
import BroFoundation
import BroPorts
import BroTestKit
import BroTestSupport
import Foundation
import Testing

/// shared/fixtures/engine/local-engine.cases.json: LocalEngine over TestCompositionRoot.
struct LocalEngineTests {
    @Test func theSharedCasesPass() async throws {
        let doc = try Fixtures.json("engine/local-engine.cases.json")
        var failures: [String] = []
        let cases = doc["cases"]?.array ?? []
        #expect(!cases.isEmpty, "local-engine.cases.json has no cases")
        for c in cases {
            let id = c["id"]?.string ?? "?"
            do {
                let run = try await LocalEngineRun(c["given"] ?? .object([]))
                do {
                    try await run.steps(c["expect"]?["steps"]?.array ?? [])
                } catch {
                    failures.append("\(id): \(error)")
                }
                await run.dispose()
            } catch {
                failures.append("\(id): \(error)")
            }
        }
        #expect(failures.isEmpty, "\(failures.joined(separator: "\n"))")
    }
}

/// A case's failure, with what went wrong.
struct CaseFailure: Error, CustomStringConvertible {
    let description: String
    init(_ description: String) { self.description = description }
}

/// A value set once from any thread.
private final class Slot<T: Sendable>: @unchecked Sendable {
    private let lock = NSLock()
    private var held: T?

    var value: T? { lock.withLock { held } }

    func set(_ value: T) { lock.withLock { held = value } }
}

/// One case: the test root as given, then its steps.
final class LocalEngineRun: @unchecked Sendable {
    static let patience: TimeInterval = 10
    static let seeded = "00000000-0000-4000-8000-1000000000"
    static let nobody = Id("00000000-0000-4000-8000-0000000000ff")
    static let marker = "/.bromelia-library"
    static let defaultScan = [
        "DRV:0,2,999,12,\"BD-RE HL-DT-ST BD-RE  WH16NS60 1.02 KLAM6E84325\",\"MOVIE_DISC\",\"/dev/rdisk4\"",
        "DRV:1,0,999,0,\"DVD+R-DL MATSHITA DVD-R   UJ8C2 SB00\",\"\",\"/dev/rdisk6\"",
    ]

    private let r: TestCompositionRoot
    private var jobs: [String: Id] = [:]
    private var feeds: [String: EventFeed] = [:]
    private var seqs: [String: Int64] = [:]
    private let released = Slot<(executorStopped: Bool, processesRunning: Int)>()
    private var stopping: Task<Void, any Error>?

    static func markerText(_ id: String, _ name: String) -> [UInt8] {
        Array("{\n  \"id\": \"m-\(id)\",\n  \"name\": \"\(name)\",\n  \"created\": \"2026-10-01T12:00:00.000Z\"\n}\n".utf8)
    }

    init(_ given: JsonValue) async throws {
        r = try await TestCompositionRoot(makemkvcon: given["makemkvcon"]?.bool ?? true, makemkvconVersion: given["makemkvVersion"]?.string,
                                          mkvmerge: given["mkvmerge"]?.bool ?? true, environment: [:],
                                          streamCapacity: Int(given["streamCapacity"]?.int ?? 1024))
        let fs = r.fs
        let config = given["config"] ?? .object([("version", .integer(3)), ("libraries", .array([
            .object([("id", .string("lib")), ("name", .string("Library")), ("path", .string("/root/library"))]),
        ]))])
        fs.put(TestCompositionRoot.configPath, JsonValue.encodeCanonical(config))
        if given["configUnreadable"]?.bool == true {
            fs.failReads(TestCompositionRoot.configPath, Self.fsFailed("read", TestCompositionRoot.configPath))
        }
        fs.put("/root/library" + Self.marker, Self.markerText("lib", "Library"))
        for (folder, library) in given["markers"]?.members ?? [] {
            fs.put(folder + Self.marker, Self.markerText(library.string ?? "", library.string ?? ""))
        }
        for folder in given["folders"]?.array ?? [] { fs.putFolder(folder.string ?? "") }
        if given["slowMarkerRead"]?.bool == true {
            // recovery reads it too, before libraryService.start
            fs.onRead = { path in if path.hasSuffix(LocalEngineRun.marker) { Thread.sleep(forTimeInterval: 0.3) } }
        }

        r.drives.add(OsDrive(device: "/dev/disk4", identification: "HL-DT-ST BD-RE WH16NS60", mountPath: "/Volumes/MOVIE_DISC"))
        r.drives.add(OsDrive(device: "/dev/disk6", identification: "MATSHITA DVD-R UJ8C2"))
        for (device, content) in given["content"]?.members ?? [] {
            guard let c = content.string.flatMap(DiscContent.init(rawValue:)) else { throw CaseFailure("content \(content)") }
            r.drives.setContent(device, c)
        }

        let scan = given["scan"]?.array?.map { $0.string ?? "" } ?? Self.defaultScan
        let listing = Fixtures.generatedListing(.object([("volume", .string("MOVIE_DISC")), ("type", .string("bluray")), ("titles", .array([
            .object([("duration", .string("1:40:00")), ("source", .integer(1)), ("chapters", .integer(12)), ("size", .integer(30_000_000_000))]),
        ]))])).split(separator: "\n").map(String.init)
        let slowScan = Slot<Bool>()
        if given["slowDriveScan"]?.bool != true { slowScan.set(true) }
        r.launcher.linesFor = { spec in
            if spec.arguments.contains("disc:9999") {
                if slowScan.value == nil {
                    slowScan.set(true)
                    Thread.sleep(forTimeInterval: 0.3)
                }
                return scan
            }
            return Self.verb(spec) == "info" ? listing : []
        }
        let holds = Set((given["holds"]?.array ?? []).compactMap(\.string))
        r.launcher.runsUntilStopped = { spec in
            !spec.arguments.contains("disc:9999") && Self.verb(spec).map(holds.contains) == true
        }
        if let takes = given["stopTakes"]?.double { r.launcher.stopTakes = .milliseconds(Int64(takes * 1000)) }
        let root = r, released = released
        r.onRelease = {
            let executor = root.executor!
            released.set((executorStopped: !executor.post({}), processesRunning: root.launcher.running()))
        }

        var n = 0
        let created = Instant(unixMilliseconds: r.clock.now().unixMilliseconds - 3_600_000)
        for i in 0..<Int(given["finishedJobs"]?.int ?? 0) {
            n += 1
            try await seed(Id(Self.seeded + Self.hex2(n)), "Done \(i)", .finished, nil, nil, false, [],
                           Instant(unixMilliseconds: created.unixMilliseconds + Int64(i) * 1000))
        }
        for j in given["jobs"]?.array ?? [] {
            n += 1
            let id = Id(Self.seeded + Self.hex2(n))
            guard let name = j["name"]?.string, let title = j["title"]?.string,
                  let state = j["state"]?.string.flatMap(JobState.init(rawValue:)) else { throw CaseFailure("job \(j)") }
            jobs[name] = id
            try await seed(id, title, state, j["driveId"]?.string, j["mediaGeneration"]?.int, j["automatic"]?.bool == true, j["rows"]?.array ?? [],
                           Instant(unixMilliseconds: created.unixMilliseconds + 500_000 + Int64(n) * 1000))
        }
    }

    private static func hex2(_ n: Int) -> String {
        let h = String(n, radix: 16)
        return h.count < 2 ? "0" + h : h
    }

    static func fsFailed(_ op: String, _ path: String) -> BroError {
        BroError("fs.failed", [("operation", .string(op)), ("path", .string(path)), ("reason", .string("injected"))])
    }

    /// The makemkvcon command of a run: info, mkv or backup.
    static func verb(_ spec: ProcessSpec) -> String? { spec.arguments.first { $0 == "info" || $0 == "mkv" || $0 == "backup" } }

    private func seed(_ id: Id, _ title: String, _ state: JobState, _ driveId: String?, _ generation: Int64?, _ automatic: Bool, _ rows: [JsonValue],
                      _ created: Instant) async throws {
        let request = JobRequest(kind: .videoDisc, title: title, automatic: automatic, driveId: driveId, mediaGeneration: generation)
        let finished = state == .finished
        let record = JobRecord(id: id, kind: .videoDisc, state: state, outcome: finished ? .succeeded : nil, queue: .acquisition, position: 0,
                               driveId: driveId, mediaGeneration: generation, automatic: automatic, title: title, request: request,
                               createdAt: created, finishedAt: finished ? created : nil)
        try await r.store.jobs().insert(record)
        for (seq, row) in rows.enumerated() {
            guard let kind = row["kind"]?.string.flatMap(StepKind.init(rawValue:)),
                  let rowState = row["state"]?.string.flatMap(StepState.init(rawValue:)) else { throw CaseFailure("row \(row)") }
            try await r.store.steps().save(StepRecord(jobId: id, seq: seq, kind: kind, state: rowState, attempt: 1, startedAt: created,
                                                      finishedAt: rowState == .running ? nil : created,
                                                      output: rowState == .succeeded ? StepOutput(kind: kind, data: .object([])) : nil))
        }
    }

    func dispose() async {
        for f in feeds.values { f.close() }
        await r.dispose()
    }

    // MARK: Steps

    func steps(_ steps: [JsonValue]) async throws {
        for (i, step) in steps.enumerated() {
            do {
                try await self.step(step)
            } catch {
                throw CaseFailure("step \(i) \(step): \(error)")
            }
        }
    }

    private func step(_ step: JsonValue) async throws {
        if let op = step["call"]?.string {
            try await call(op, step)
            return
        }
        guard let (key, value) = step.members?.first else { throw CaseFailure("an empty step") }
        let r = r
        switch key {
        case "start":
            _ = try await expect(step) {
                try await r.start()
                return nil
            }
        case "stop":
            try await within("stop", patience: 40) { try await r.stop() }
        case "stopBegins":
            stopping = Task { try await r.stop() }
            try await Task.sleep(nanoseconds: 50_000_000)
        case "stopEnds":
            guard let stopping else { throw CaseFailure("no stop began") }
            try await within("stop") { try await stopping.value }
        case "monitoring":
            if r.drives.monitoring() != value.bool { throw CaseFailure("monitoring \(r.drives.monitoring())") }
        case "timers":
            if Int64(r.clock.pending()) != value.int { throw CaseFailure("timers pending \(r.clock.pending())") }
        case "restart":
            try await within("restart", patience: 40) { try await r.restart() }
        case "settle":
            try await settle()
        case "engineLog":
            try same(value, .array(r.log.lines().map(Self.line)), "engine log", exact: true)
        case "processes":
            let runs = r.launcher.specs().filter { !$0.arguments.contains("disc:9999") }.map { s -> JsonValue in
                let source = s.arguments.first { $0.hasPrefix("disc:") || $0.hasPrefix("iso:") || $0.hasPrefix("file:") }
                return .string((Self.verb(s) ?? "") + " " + (source ?? ""))
            }
            try same(value, .array(runs), "processes", exact: true)
        case "trayCalls":
            try same(value, .array(r.drives.calls().map(JsonValue.string)), "tray calls", exact: true)
        case "files":
            for p in value["exist"]?.array ?? [] where !r.fs.exists(subst(p.string ?? "")) { throw CaseFailure("missing " + subst(p.string ?? "")) }
            for p in value["absent"]?.array ?? [] where r.fs.exists(subst(p.string ?? "")) { throw CaseFailure("there " + subst(p.string ?? "")) }
        case "stored":
            for (name, want) in value.members ?? [] {
                guard let id = jobs[name], let record = try await r.store.jobs().load(id) else { throw CaseFailure("no record of " + name) }
                let q = record.request
                let request = JsonValue.object([("kind", .string(q.kind.rawValue)), ("title", .string(q.title)), ("automatic", .bool(q.automatic)),
                                                ("driveId", q.driveId.map(JsonValue.string) ?? .null),
                                                ("mediaGeneration", q.mediaGeneration.map(JsonValue.integer) ?? .null), ("options", q.options ?? .null)])
                try same(want, .object([("mediaGeneration", record.mediaGeneration.map(JsonValue.integer) ?? .null), ("request", request)]),
                         "stored " + name)
            }
        case "released":
            guard let seen = released.value else { throw CaseFailure("release never ran") }
            try same(value, .object([("executorStopped", .bool(seen.executorStopped)), ("processesRunning", .integer(Int64(seen.processesRunning)))]),
                     "released")
        case "feed":
            let after: Int64?
            switch value["after"] {
            case .string(let s)?: after = Int64(subst(s))
            case .integer(let a)?: after = a
            default: after = nil
            }
            let jobLog = try (value["jobLog"]?.array ?? []).map { j -> Id in
                guard let id = j.string.flatMap({ jobs[$0] }) else { throw CaseFailure("no job \(j)") }
                return id
            }
            guard let name = step["as"]?.string else { throw CaseFailure("a feed without as") }
            let engine = r.engine!
            feeds[name] = try await within("events") { try await engine.events(after: after, jobLog: jobLog) }
        case "next":
            guard let feed = value.string.flatMap({ feeds[$0] }) else { throw CaseFailure("no feed \(value)") }
            for want in step["expect"]?.array ?? [] {
                let e = try await within("the next event") { try await feed.next() }
                if want.isNull {
                    if let e { throw CaseFailure("an event after the feed closed: \(EngineEvent.json(e))") }
                    continue
                }
                guard let e else { throw CaseFailure("the feed ended") }
                let json = EngineEvent.json(e)
                let members = (want.members ?? []).filter { !["seqIsLast", "jobCount", "firstJob"].contains($0.key) }
                try same(subst(.object(members)), json, "event")
                let last = await r.bus.lastSeq
                if want["seqIsLast"]?.bool == true && e.seq != last { throw CaseFailure("event seq \(e.seq), last \(last)") }
                let snapshotJobs = json["data"]?["jobs"]?.array
                if let count = want["jobCount"]?.int, snapshotJobs.map({ Int64($0.count) }) != count {
                    throw CaseFailure("snapshot jobs \(snapshotJobs?.count ?? -1), wanted \(count)")
                }
                if let first = want["firstJob"]?.string, snapshotJobs?.first?["id"]?.string != jobs[first]?.value {
                    throw CaseFailure("first snapshot job \(snapshotJobs?.first?["id"] ?? .null)")
                }
            }
        case "drain":
            guard let feed = value.string.flatMap({ feeds[$0] }) else { throw CaseFailure("no feed \(value)") }
            var logsOf = Set<String>()
            while true {
                guard let next = await outcome(0.3, { try await feed.next() }), let e = try next.get() else { break }
                if e.type == .jobLog, let job = e.data["jobId"]?.string { logsOf.insert(job) }
            }
            let want = Set((step["jobLogsOf"]?.array ?? []).compactMap { $0.string.flatMap { jobs[$0]?.value } })
            if want != logsOf { throw CaseFailure("jobLog events of " + logsOf.sorted().joined(separator: ", ")) }
        case "seq":
            guard let name = value.string else { throw CaseFailure("seq \(value)") }
            seqs[name] = await r.bus.lastSeq
        case "fail":
            guard let path = value["path"]?.string, let op = value["op"]?.string else { throw CaseFailure("fail \(value)") }
            r.fs.fail(op, path, Self.fsFailed(op, path))
        case "unfail":
            r.fs.clearFails()
        case "unmount":
            try await r.unmount(value.string ?? "")
        case "mount":
            try await r.mount(value.string ?? "")
        case "limitsOf":
            guard let config = value["config"], let cores = value["cores"]?.int else { throw CaseFailure("limitsOf \(value)") }
            let limits = SchedulerLimits.of(try ConfigCodec.decode(JsonValue.encodeCanonical(config)), cores: Int(cores))
            try same(step["expect"] ?? .null, .object([("acquisitions", .integer(Int64(limits.acquisitions))), ("processing", .integer(Int64(limits.processing))),
                                                       ("cpu", .integer(Int64(limits.cpu))), ("ioPerVolume", .integer(Int64(limits.ioPerVolume)))]),
                     "limits", exact: true)
        case "systemSeqIsLast":
            let engine = r.engine!
            let status = try await within("system") { try await engine.system() }
            let last = await r.bus.lastSeq
            if status.eventSeq != last { throw CaseFailure("system eventSeq \(status.eventSeq), last \(last)") }
        default:
            throw CaseFailure("unknown step " + key)
        }
    }

    private static func errorJson(_ e: BroError) -> JsonValue {
        e.params.isEmpty ? .object([("code", .string(e.code))]) : .object([("code", .string(e.code)), ("params", .object(e.params))])
    }

    private static func line(_ m: BroMessage) -> JsonValue {
        m.params.isEmpty ? .object([("code", .string(m.code.rawValue))]) : .object([("code", .string(m.code.rawValue)), ("params", .object(m.params))])
    }

    private func job(_ name: JsonValue) -> Id { name.string.flatMap { jobs[$0] } ?? Self.nobody }

    private func call(_ op: String, _ step: JsonValue) async throws {
        let a = step["args"]?.array ?? []
        func arg(_ i: Int) throws -> JsonValue {
            guard i < a.count else { throw CaseFailure("\(op) needs argument \(i)") }
            return a[i]
        }
        func text(_ i: Int) throws -> String {
            guard let s = try arg(i).string else { throw CaseFailure("\(op)'s argument \(i) isn't a string") }
            return s
        }
        func int(_ i: Int) throws -> Int64 {
            guard let n = try arg(i).int else { throw CaseFailure("\(op)'s argument \(i) isn't a number") }
            return n
        }
        let e = r.engine!
        let call: @Sendable () async throws -> JsonValue?
        switch op {
        case "system": call = { SystemStatusDto.json(try await e.system()) }
        case "stop": call = { try await e.stop(); return nil }
        case "drives": call = { .array(try await e.drives().map(DriveDto.json)) }
        case "eject": let d = try text(0); call = { try await e.eject(d); return nil }
        case "closeTray": let d = try text(0); call = { try await e.closeTray(d); return nil }
        case "closeAllTrays": call = { try await e.closeAllTrays(); return nil }
        case "rip": let d = try text(0), o = try Self.options(arg(1)); call = { JobDto.json(try await e.rip(d, options: o)) }
        case "jobs": call = { JobPageDto.json(try await e.jobs(JobQueryDto())) }
        case "job": let j = job(try arg(0)); call = { JobDetailDto.json(try await e.job(j)) }
        case "jobLog": let j = job(try arg(0)), after = try int(1); call = { .array(try await e.jobLog(j, after: after).map(LogLineDto.json)) }
        case "cancel": let j = job(try arg(0)); call = { try await e.cancel(j); return nil }
        case "retry":
            let j = job(try arg(0)), k = try arg(1).string.flatMap(StepKind.init(rawValue:))
            call = { JobDto.json(try await e.retry(j, fromStep: k)) }
        case "keepApart": let j = job(try arg(0)); call = { try await e.keepApart(j); return nil }
        case "decide":
            let j = job(try arg(0)), d = try text(1), answer = try text(2)
            call = { try await e.decide(j, decisionId: d, answer: answer); return nil }
        case "move": let j = job(try arg(0)), by = Int(try int(1)); call = { try await e.move(j, by: by); return nil }
        case "startNow": let j = job(try arg(0)); call = { try await e.startNow(j); return nil }
        case "libraries": call = { .array(try await e.libraries().map(LibraryDto.json)) }
        case "attachLibrary": let p = try text(0), n = try text(1); call = { LibraryDto.json(try await e.attachLibrary(p, name: n)) }
        case "config": call = { ConfigEnvelope.json(try await e.config()) }
        case "updateConfig": let d = try arg(0), v = try int(1); call = { ConfigEnvelope.json(try await e.updateConfig(d, version: v)) }
        case "validateConfig": let d = try arg(0); call = { .array(try await e.validateConfig(d).map { $0.toJson() }) }
        case "setSecret": let n = try text(0), v = try text(1); call = { try await e.setSecret(n, value: v); return nil }
        case "exportConfig": call = { try await e.exportConfig() }
        case "startJob": let q = try Self.request(arg(0)); call = { JobDto.json(try await e.startJob(q)) }
        default: throw CaseFailure("unknown operation " + op)
        }
        let result = try await expect(step, call)
        if let name = step["as"]?.string, let id = result?["id"]?.string { jobs[name] = Id(id) }
        if let includes = step["includes"]?.array {
            for want in includes where !(result?.array ?? []).contains(where: { matches(subst(want), $0) }) {
                throw CaseFailure("no line like \(want) in \(result ?? .null)")
            }
        }
    }

    private static func options(_ o: JsonValue) throws -> RipOptionsDto {
        RipOptionsDto(mode: o["mode"]?.string.flatMap(RipMode.init(rawValue:)), profile: o["profile"]?.string, library: o["library"]?.string,
                      titles: o["titles"]?.array.map { $0.compactMap { $0.int.map(Int.init) } }, outputFolder: o["outputFolder"]?.string)
    }

    private static func request(_ q: JsonValue) throws -> JobRequest {
        guard let kind = q["kind"]?.string.flatMap(JobKind.init(rawValue:)), let title = q["title"]?.string else { throw CaseFailure("request \(q)") }
        return JobRequest(kind: kind, title: title, automatic: q["automatic"]?.bool == true, driveId: q["driveId"]?.string,
                          mediaGeneration: q["mediaGeneration"]?.int, options: q["options"], details: q["details"])
    }

    /// Runs the action; checks its result against expect, or its failure against error.
    private func expect(_ step: JsonValue, _ action: @escaping @Sendable () async throws -> JsonValue?) async throws -> JsonValue? {
        let result: JsonValue?
        do {
            result = try await within("the call", action)
        } catch let f as BroError {
            guard let wantError = step["error"] else { throw CaseFailure("failed: \(Self.errorJson(f))") }
            try same(subst(wantError), Self.errorJson(f), "error")
            return nil
        }
        if let error = step["error"] { throw CaseFailure("no error, wanted \(error); got \(result ?? .null)") }
        if let want = step["expect"], !want.isNull { try same(subst(want), result ?? .null, "result") }
        return result
    }

    /// The operation's result; a failure when it hasn't ended after `patience` seconds. The operation runs on as a task of
    /// its own.
    private func within<T: Sendable>(_ what: String, patience: TimeInterval = LocalEngineRun.patience,
                                     _ op: @escaping @Sendable () async throws -> T) async throws -> T {
        guard let result = await outcome(patience, op) else { throw CaseFailure(what + " never ended") }
        return try result.get()
    }

    /// The operation's result or failure; nil when it hasn't ended after `patience` seconds (it runs on as a task of its
    /// own).
    private func outcome<T: Sendable>(_ patience: TimeInterval, _ op: @escaping @Sendable () async throws -> T) async -> Result<T, any Error>? {
        let done = Slot<Result<T, any Error>>()
        Task {
            do {
                done.set(.success(try await op()))
            } catch {
                done.set(.failure(error))
            }
        }
        let deadline = Date().addingTimeInterval(patience)
        while Date() <= deadline {
            if let result = done.value { return result }
            try? await Task.sleep(nanoseconds: 1_000_000)
        }
        return done.value
    }

    /// Until the executor's activity is the held processes' (each one a running step's worker), seen three times in a row.
    private func settle() async throws {
        let deadline = Date().addingTimeInterval(Self.patience)
        var seen = 0
        while seen < 3 {
            if Date() > deadline { throw CaseFailure("never settled: activity \(r.executor.activity), held \(r.launcher.held())") }
            try await Task.sleep(nanoseconds: 5_000_000)
            seen = r.executor.activity == r.launcher.held() ? seen + 1 : 0
        }
    }

    // MARK: Matching

    private func subst(_ v: JsonValue) -> JsonValue {
        switch v {
        case .string(let s): return .string(subst(s))
        case .array(let a): return .array(a.map(subst))
        case .object(let m): return .object(m.map { ($0.key, subst($0.value)) })
        default: return v
        }
    }

    private func subst(_ s: String) -> String {
        var s = s
        for (name, id) in jobs { s = s.replacingOccurrences(of: "{\(name)}", with: id.value) }
        for (name, seq) in seqs { s = s.replacingOccurrences(of: "{\(name)}", with: String(seq)) }
        return s
    }

    private func same(_ want: JsonValue, _ got: JsonValue, _ what: String, exact: Bool = false) throws {
        if !matches(want, got, exact: exact) { throw CaseFailure("\(what): wanted \(want)\n got \(got)") }
    }

    /// Objects: every wanted member matches (a wanted null: absent or null); exact also wants no others. Arrays: the same
    /// length, element by element. Anything else: equal.
    private func matches(_ want: JsonValue, _ got: JsonValue?, exact: Bool = false) -> Bool {
        switch want {
        case .null:
            return got == nil || got?.isNull == true
        case .object(let o):
            guard let g = got?.members else { return false }
            if exact && g.count != o.count { return false }
            return o.allSatisfy { matches($0.value, got?[$0.key], exact: exact) }
        case .array(let a):
            guard let ga = got?.array, ga.count == a.count else { return false }
            return zip(a, ga).allSatisfy { matches($0, $1, exact: exact) }
        case .integer, .number:
            guard let n = got?.double else { return false }
            return want.double == n
        default:
            return want == got
        }
    }
}
