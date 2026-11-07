import BroDomain
import BroFoundation

/// The one interface between the engine and anything outside it (plan §13.1): DTOs in and out, never live objects.
/// LocalEngine implements it now; RemoteEngine (phase 4) too. It holds the operations of the phases built so far: it
/// grows with each phase. Every operation fails with a BroError carrying the error the route lists; server.stopping
/// once the engine stops.
public protocol EngineAPI: Sendable {
    // MARK: System

    /// Versions, MakeMKV, tools, problems, libraries, configuration version, last event seq.
    func system() async throws -> SystemStatusDto

    /// The stop order: running jobs are left as a killed engine would leave them, for the next start's recovery.
    /// server.stopping when stopping already.
    func stop() async throws

    // MARK: Drives

    func drives() async throws -> [DriveDto]

    func eject(_ driveId: String) async throws

    func closeTray(_ driveId: String) async throws

    func closeAllTrays() async throws

    /// A manual video-disc job on the drive's disc: drive.notFound, drive.noDisc, drive.busy, disc.notVideo; titles:
    /// session.notOpen until SessionService; an outputFolder that is no library's: library.notAttached.
    func rip(_ driveId: String, options: RipOptionsDto) async throws -> JobDto

    // MARK: Jobs

    func jobs(_ query: JobQueryDto) async throws -> JobPageDto

    func job(_ jobId: Id) async throws -> JobDetailDto

    func jobLog(_ jobId: Id, after: Int64) async throws -> [LogLineDto]

    func cancel(_ jobId: Id) async throws

    func retry(_ jobId: Id, fromStep: StepKind?) async throws -> JobDto

    /// Nothing returned (204) once queued.
    func keepApart(_ jobId: Id) async throws

    func decide(_ jobId: Id, decisionId: String, answer: String) async throws

    func move(_ jobId: Id, by: Int) async throws

    func startNow(_ jobId: Id) async throws

    // MARK: Libraries

    func libraries() async throws -> [LibraryDto]

    /// Writes the folder's marker and adds the library to the configuration; if that save fails the marker is removed
    /// again and the save's error returned.
    func attachLibrary(_ path: String, name: String) async throws -> LibraryDto

    // MARK: Configuration

    func config() async throws -> ConfigEnvelope

    func updateConfig(_ doc: JsonValue, version: Int64) async throws -> ConfigEnvelope

    func validateConfig(_ doc: JsonValue) async throws -> [Issue]

    func setSecret(_ name: String, value: String) async throws

    /// The ExportBundle document.
    func exportConfig() async throws -> JsonValue

    // MARK: Events

    /// The event feed from `after` (nil: a snapshot first); jobLog events only for the jobs in `jobLog`.
    func events(after: Int64?, jobLog: [Id]) async throws -> EventFeed
}
