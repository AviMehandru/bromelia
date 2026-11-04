import BroAdapters
import BroDomain
import BroEngine
import BroFoundation
import BroPorts
import Bromeliad
import Foundation
import Testing

/// Decision 301's twin on macOS (review L3-7): the key a crash left in an unfinished job's home is scrubbed by the real
/// root's startup. HomeDirIsolation's own cases and recovery-service's unreleased-homes-are-scrubbed-and-the-registry-put-back
/// pin the scrub itself; this pins that the root hands RecoveryService the same isolation and job homes
/// (<data>/jobs/<id>/home) MakemkvTool runs in. The job row is written through SqliteStore before the build, and the
/// settings.conf holds a test key only: never a real one. A temporary data folder and home, no system keystore or tools, an
/// empty environment.
struct HomeScrubTests {
    @Test func aKeyACrashLeftInAJobsHomeIsScrubbedWhenTheRealRootStarts() async throws {
        let root = URL(fileURLWithPath: NSTemporaryDirectory()).resolvingSymlinksInPath().path + "/bromelia-host-" + UUID().uuidString
        defer { try? FileManager.default.removeItem(atPath: root) }
        let data = root + "/data", home = root + "/home"
        try FileManager.default.createDirectory(atPath: home, withIntermediateDirectories: true)
        try FileManager.default.createDirectory(atPath: data, withIntermediateDirectories: true)
        let id = Id("00000000-0000-4000-8000-000000000001")
        let folder = data + "/jobs/" + id.value + "/home/Library/MakeMKV"
        try FileManager.default.createDirectory(atPath: folder, withIntermediateDirectories: true)
        let conf = folder + "/settings.conf"
        try "app_DataDir = \"\(folder)\"\napp_Key = \"T-BromeliaTest-not-a-key\"\napp_UpdateEnable = \"0\"\n".write(toFile: conf, atomically: true, encoding: .utf8)

        let clock = SystemClock()
        let store = try SqliteStore.open(data + "/bromelia.db", clock: clock)
        try await store.migrate()
        let now = clock.now()
        let request = JobRequest(kind: .videoDisc, title: "Sample Movie", automatic: false, driveId: nil, mediaGeneration: nil)
        try await store.jobs().insert(JobRecord(id: id, kind: .videoDisc, state: .running, outcome: nil, queue: .acquisition, position: 0,
                                                driveId: nil, mediaGeneration: nil, automatic: false, title: "Sample Movie", request: request,
                                                createdAt: now, finishedAt: nil))
        try await store.steps().save(StepRecord(jobId: id, seq: 0, kind: .probe, state: .failed, attempt: 1, startedAt: now, finishedAt: now))
        store.close()

        let engine = try await CompositionRoot.build(RootOptions(dataDir: data, configPath: nil, home: home, systemKeystore: false,
                                                                 systemTools: false, drives: false, environment: [:]))
        try await engine.start()
        let text = try String(contentsOfFile: conf, encoding: .utf8)
        #expect(!text.contains("app_Key"), "the key is scrubbed")
        #expect(text.contains("app_UpdateEnable = \"0\""), "the other settings stay")
        // No makemkv.keyNotRemoved, raised or in the job's log.
        let problems = try await engine.system().problems.map { $0.message.code.rawValue }
        #expect(!problems.contains("makemkv.keyNotRemoved"))
        let log = (try? String(contentsOfFile: data + "/jobs/" + id.value + "/log.jsonl", encoding: .utf8)) ?? ""
        #expect(!log.contains("makemkv.keyNotRemoved"))
        try await engine.stop()
    }
}
