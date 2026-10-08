import BroAdapters
import BroDomain
import BroFoundation
import BroPorts
import Foundation
import SQLite3
import Testing

/// What a full disk does to Bromelia's writes (review follow-ups, failure testing; the same checks as Linux's
/// /full-disk/* and Windows' FullDiskTests). Opt-in: BROMELIA_TEST_FULL_DIR names an empty folder on a small volume of
/// its own (a 32 MB disk image); each test fills it around the step it checks.
@Suite(.serialized) struct FullDiskTests {
    let dir = ProcessInfo.processInfo.environment["BROMELIA_TEST_FULL_DIR"].flatMap { $0.isEmpty ? nil : $0 }

    func freeBytes(_ dir: String) -> UInt64 {
        var v = statvfs()
        precondition(statvfs(dir, &v) == 0)
        return UInt64(v.f_bavail) * UInt64(v.f_frsize)
    }

    /// Fills the volume until at most `leave` bytes are free; returns the filler's path (remove it afterwards).
    func fill(_ dir: String, leave: UInt64) -> String {
        let filler = dir + "/filler.bin"
        let fd = open(filler, O_WRONLY | O_CREAT | O_TRUNC, 0o644)
        precondition(fd >= 0)
        let block = [UInt8](repeating: 0, count: 64 * 1024)
        while freeBytes(dir) > leave {
            let n = min(block.count, Int(freeBytes(dir) - leave))
            if block.withUnsafeBytes({ write(fd, $0.baseAddress!, n) }) <= 0 { break }
            fsync(fd)
        }
        close(fd)
        return filler
    }

    func names(in dir: String) -> [String] {
        ((try? FileManager.default.contentsOfDirectory(atPath: dir)) ?? []).filter { $0 != "filler.bin" }.sorted()
    }

    /// A data disc bigger than the free space: the copy fails and leaves nothing, never a partial disc.iso.
    @Test func aDataDiscThatDoesntFitLeavesNothing() async throws {
        guard let dir else { return }
        let drives = FakeDriveControl()
        drives.disc = FakeDisc()
        drives.disc!.sectors = Int64(freeBytes(dir) / 2048) + 4096
        do {
            _ = try await DataImager(drives: drives, fs: PlatformFileSystem()).copy("/dev/rdisk9", destIso: dir + "/disc.iso", sink: NoSink(),
                                                                                    cancel: CancellationSource().token)
            Issue.record("the copy succeeded")
        } catch {
            #expect(error.code == "fs.failed")
        }
        #expect(names(in: dir) == [])
    }

    /// A full disk under writeAtomically: it fails, the old file is still whole, no temporary file is left.
    @Test func writeAtomicallyKeepsTheOldFile() throws {
        guard let dir else { return }
        let fs = PlatformFileSystem(), path = dir + "/r.json"
        try fs.writeAtomically(path, bytes: Array("old".utf8), mode: 0o644)
        let filler = fill(dir, leave: 64 * 1024)
        defer { unlink(filler); unlink(path) }
        #expect(throws: BroError.self) { try fs.writeAtomically(path, bytes: [UInt8](repeating: 0, count: 1 << 20), mode: 0o644) }
        #expect(try String(contentsOfFile: path, encoding: .utf8) == "old")
        #expect(names(in: dir) == ["r.json"])
    }

    /// A transcript that fills the disk: the tool runs to the end, every line still reaches the reader, and the run
    /// reports process.noTranscript.
    @Test func aTranscriptThatFillsTheDiskIsReported() async throws {
        guard let dir else { return }
        let filler = fill(dir, leave: 32 * 1024), transcript = dir + "/t.txt"
        defer { unlink(filler); unlink(transcript) }
        var spec = ProcessSpec(executable: "/bin/sh", arguments: ["-c", "i=0; while [ $i -lt 20000 ]; do echo line-$i-0123456789012345678901234567890123456789; i=$((i+1)); done"],
                               environment: [:], stopPolicy: .interruptFirst)
        spec.transcript = transcript
        let p = try PlatformProcessLauncher().start(spec)
        let reader = Task { var n = 0; for await _ in p.lines() { n += 1 }; return n }
        let exit = await p.wait()
        #expect(exit.status == 0)
        #expect(await reader.value == 20000)
        #expect(p.transcriptProblem()?.code == .processNoTranscript)
    }

    /// The database on a full disk: a write fails with store.failed, and once there is space again the database opens,
    /// is intact, and still has what was written before.
    @Test func theDatabaseSurvivesAFullDisk() async throws {
        guard let dir else { return }
        let path = dir + "/s.sqlite"
        let store = try SqliteStore.open(path, clock: FixedClock())
        try await store.migrate()
        try await store.kv().set("before", value: .string("kept"))
        let filler = fill(dir, leave: 48 * 1024)
        var failure: BroError?
        let pad = String(repeating: "x", count: 8000)
        for i in 0..<2000 where failure == nil {
            do { try await store.kv().set("k\(i)", value: .string(pad)) } catch { failure = error }
        }
        store.close()
        unlink(filler)
        #expect(failure?.code == "store.failed")
        let reopened = try SqliteStore.open(path, clock: FixedClock())
        #expect(try await reopened.kv().get("before") == .string("kept"))
        reopened.close()
        var db: OpaquePointer?
        var stmt: OpaquePointer?
        #expect(sqlite3_open(path, &db) == SQLITE_OK)
        #expect(sqlite3_prepare_v2(db, "PRAGMA integrity_check", -1, &stmt, nil) == SQLITE_OK)
        #expect(sqlite3_step(stmt) == SQLITE_ROW)
        #expect(String(cString: sqlite3_column_text(stmt, 0)) == "ok")
        sqlite3_finalize(stmt)
        sqlite3_close(db)
        for name in (try? FileManager.default.contentsOfDirectory(atPath: dir)) ?? [] { unlink(dir + "/" + name) }
    }
}

private struct NoSink: RunSink { func event(_ event: RobotEvent) {} }
