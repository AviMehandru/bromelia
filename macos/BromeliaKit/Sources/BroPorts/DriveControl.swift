import BroDomain
import BroFoundation

/// Trays, mounts and raw access.
public protocol DriveControl: Sendable {
    func eject(_ device: String) async throws(BroError)

    func closeTray(_ device: String) async throws(BroError)

    /// The mount path; none after timeout or when cancelled.
    func waitForMount(_ device: String, timeout: Duration, cancel: CancellationToken) async -> String?

    func probeContent(_ device: String) -> DiscContent

    /// Whole 2048-byte sectors of a drive's disc or an image file; fails when the size can't be read (drive.sizeUnknown) or
    /// isn't a whole number of sectors (drive.partialSector).
    func openRaw(_ device: String) throws(BroError) -> any SectorReader
}
