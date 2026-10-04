import BroDomain
import BroFoundation

/// A day's counters of drive_stats_daily.
public struct DriveStats: Sendable, Equatable {
    public var jobs: Int
    public var failedJobs: Int
    public var readErrorJobs: Int
    public var readErrors: Int
    public var bytesRead: Int64
    public var secondsReading: Double

    public init(jobs: Int, failedJobs: Int, readErrorJobs: Int, readErrors: Int, bytesRead: Int64,
                secondsReading: Double) {
        self.jobs = jobs
        self.failedJobs = failedJobs
        self.readErrorJobs = readErrorJobs
        self.readErrors = readErrors
        self.bytesRead = bytesRead
        self.secondsReading = secondsReading
    }
}
