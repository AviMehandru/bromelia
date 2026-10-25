import BroFoundation
import BroPorts

/// What a step may do with a library (plan §12.2), implemented by LibraryService.
public protocol LibraryPort: Sendable {
    /// library.notFound, or the reason it is offline (library.offline when its marker is missing).
    func require(_ libraryId: String) throws(BroError) -> LibraryHandle

    /// Where the job's files are made: <library>/.bromelia/staging/<job> (the same file system, so the commit is a
    /// rename), made when it isn't there; never the library's root.
    func stagingDirectory(_ libraryId: String, jobId: Id) throws(BroError) -> String

    /// Plan §22.3 steps 4–6 (Seal did 1–3): the intent, the move (a new folder renamed whole; a shared one item by item,
    /// SHA256SUMS merged), the folders synced, then the unit committed with its files. It first waits while another
    /// commit or roll forward of the unit runs; `cancel` ends only that wait (job.cancelled). An open intent of the unit is
    /// finished instead, and a unit committed already gives its result again. Off the executor.
    func commit(_ unit: UnitRecord, intent: CommitIntent, files: [UnitFile], cancel: CancellationToken) async throws(BroError) -> CommitResult

    /// `commit` into <library>/_Quarantine/<name> [<tag>] (a free name; name: the last part of the intent's destination),
    /// not merged, the unit quarantined.
    func quarantine(_ unit: UnitRecord, intent: CommitIntent, files: [UnitFile], tag: String, cancel: CancellationToken) async throws(BroError) -> CommitResult
}
