import BroDomain
import BroFoundation

/// drives and drive_stats_daily.
public protocol DriveRepository: Sendable {
    func upsert(_ drive: DriveRecord) async throws(BroError)

    func all() async throws(BroError) -> [DriveRecord]

    /// Adds stats to the day's counters (day: YYYY-MM-DD).
    func recordStats(_ driveId: String, day: String, stats: DriveStats) async throws(BroError)
}
