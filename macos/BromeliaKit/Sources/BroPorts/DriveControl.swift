import BroDomain
import BroFoundation

/// Trays, mounts and raw access.
public protocol DriveControl: Sendable {
    func eject(_ device: String) async throws(BroError)

    func closeTray(_ device: String) async throws(BroError)

    /// The mount path; none after timeout or when cancelled.
    func waitForMount(_ device: String, timeout: Duration, cancel: CancellationToken) async -> String?

    func probeContent(_ device: String) -> DiscContent

    func openRaw(_ device: String) throws(BroError) -> any SectorReader
}
