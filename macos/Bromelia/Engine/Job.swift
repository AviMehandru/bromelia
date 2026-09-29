import Foundation
import Observation

enum JobState: String, Codable, Sendable {
    /// `completedWithErrors`: every title was saved, but MakeMKV reported read errors while ripping,
    /// so the files may contain damaged or skipped data. They are kept apart from finished archives.
    case queued, waiting, running, succeeded, completedWithErrors, failed, cancelled

    var isFinished: Bool { self == .succeeded || self == .completedWithErrors || self == .failed || self == .cancelled }
    var isActive: Bool { self == .running }

    var label: String {
        switch self {
        case .queued: return "Queued"
        case .waiting: return "Starting soon"
        case .running: return "Running"
        case .succeeded: return "Completed"
        case .completedWithErrors: return "Completed with read errors"
        case .failed: return "Failed"
        case .cancelled: return "Cancelled"
        }
    }

    var statusWord: String {
        switch self {
        case .succeeded: return "success"
        case .completedWithErrors: return "errors"
        case .cancelled: return "cancelled"
        default: return "failed"
        }
    }
}

struct LogEntry: Identifiable, Sendable, Hashable {
    let id: Int
    let time: Date
    let severity: RobotMessage.Severity
    let text: String
}

@MainActor
@Observable
final class RipJob: Identifiable {
    let id = UUID()
    let createdAt = Date()
    var source: DiscSource
    /// Snapshot of the drive configuration at the time the job was created.
    var drive: DriveConfig
    /// Jobs with the same lane key never run concurrently (one per physical drive).
    let laneKey: String
    var sourceLabel: String
    var discLabel: String
    var mode: RipMode
    var preloadedInfo: DiscInfo?
    /// Explicit title choice (from the disc view). nil = use the drive's title selection rules.
    var manualTitles: [Int]?
    /// Per-title track choice (stream indices). Requires mkvmerge.
    var trackSelections: [Int: Set<Int>] = [:]
    /// Explicit output file names per title (take precedence over the file name template).
    var titleNameOverrides: [Int: String] = [:]
    /// Movie / show name typed by the user. Empty = inferred from the disc.
    var mediaName = ""
    /// Movie or TV show, as chosen by the user. nil = inferred.
    var mediaKind: MediaKind?
    /// Number of the first episode on this disc, as entered by the user. nil = read from the menus or 1.
    var firstEpisode: Int?
    /// File system flags from the drive scan (used when the disc listing is unavailable).
    var discFlags: DiscFlags?
    var isAutomatic = false
    /// The history's successful jobs with their disc fingerprints, to recognise a disc archived before (set when it starts).
    var archivedCandidates: [ArchivedCandidate] = []
    /// The disc's fingerprint (DiscFingerprint), once the job has read the listing.
    var fingerprint: String?
    /// Delayed start (automatic rips): the job waits in `.waiting` until this time.
    var startAt: Date?

    // Live state
    var state: JobState = .queued
    var phase = "Queued"
    var currentOperation = ""
    var totalOperation = ""
    var currentProgress = 0.0
    var totalProgress = 0.0
    var stepIndex = 0
    var stepCount = 1
    var startedAt: Date?
    var finishedAt: Date?
    var outputDirectory: URL?
    var producedFiles: [URL] = []
    var discInfo: DiscInfo?
    var errorMessage: String?
    var warningCount = 0
    var errorCount = 0
    var log: [LogEntry] = []
    var commands: [String] = []
    var ripTitles: [Int] = []
    var identity: MediaIdentity?
    var episodes: [ArchiveRecord.Episode] = []
    var checksums: [Checksums.Entry] = []
    var checksumFile: URL?
    var makemkvVersion = ""
    var errorMessages: [String] = []
    /// Errors MakeMKV reported while ripping or backing up (read errors, hash check failures, ...).
    var dataErrors: [String] = []
    /// "Using LibreDrive mode (…)" details, when the drive read the disc in LibreDrive mode.
    var libreDrive: String?
    /// A problem with MakeMKV or the drive that explains a failure (expired key, LibreDrive required, …).
    var makemkvProblem: MakeMKVNotice?
    /// The listing the titles were ripped from (the disc's, a backup's, or a one-pass listing).
    var ripInfo: DiscInfo?
    /// The job was started with the drive's configured mode (automatic and quick rips), so the mode may follow
    /// the disc's format (`rip.formatModes`).
    var usesConfiguredMode = false
    /// The movie / show found online.
    var metadata: MediaMatch?
    /// Post-processing steps that run after the job, in the background queue.
    var background: BackgroundWork?

    @ObservationIgnored private var nextLogId = 0
    @ObservationIgnored var logHandle: FileHandle?
    static let maxLogEntries = 20_000

    init(source: DiscSource, drive: DriveConfig, laneKey: String, sourceLabel: String, discLabel: String, mode: RipMode) {
        self.source = source
        self.drive = drive
        self.laneKey = laneKey
        self.sourceLabel = sourceLabel
        self.discLabel = discLabel
        self.mode = mode
    }

    var title: String {
        let disc = discLabel.isEmpty ? "Disc" : discLabel
        return "\(disc) — \(sourceLabel)"
    }

    var overallProgress: Double {
        if state == .succeeded { return 1 }
        let v = (Double(stepIndex) + min(max(totalProgress, 0), 1)) / Double(max(stepCount, 1))
        return min(max(v, 0), 1)
    }

    var elapsed: TimeInterval {
        guard let s = startedAt else { return 0 }
        return (finishedAt ?? Date()).timeIntervalSince(s)
    }

    /// Estimated remaining time, extrapolated from the overall progress.
    var estimatedRemaining: TimeInterval? {
        let p = overallProgress
        guard state == .running, p > 0.02, elapsed > 10 else { return nil }
        return elapsed / p * (1 - p)
    }

    var logDirectory: URL { Paths.jobsDirectory.appendingPathComponent(id.uuidString, isDirectory: true) }
    var logFile: URL { logDirectory.appendingPathComponent("log.txt") }
    var manifestFile: URL { logDirectory.appendingPathComponent("manifest.json") }

    func appendLog(_ text: String, severity: RobotMessage.Severity = .info) {
        let entry = LogEntry(id: nextLogId, time: Date(), severity: severity, text: text)
        nextLogId += 1
        log.append(entry)
        if log.count > RipJob.maxLogEntries + 2_000 { log.removeFirst(log.count - RipJob.maxLogEntries) }
        if severity == .warning { warningCount += 1 }
        if severity == .error { errorCount += 1 }
        if let h = logHandle {
            let stamp = RipJob.logTimeFormatter.string(from: entry.time)
            let tag = severity == .info ? "" : "[\(severity.rawValue.uppercased())] "
            h.write(Data("\(stamp) \(tag)\(text)\n".utf8))
        }
    }

    static let logTimeFormatter: DateFormatter = {
        let f = DateFormatter()
        f.dateFormat = "yyyy-MM-dd HH:mm:ss"
        return f
    }()
}

/// Persisted summary of a finished job.
struct HistoryRecord: Codable, Identifiable, Hashable, Sendable {
    var id: UUID
    var title: String
    var driveName: String
    var discName: String
    var mode: RipMode
    var state: JobState
    var startedAt: Date?
    var finishedAt: Date?
    var outputDirectory: String?
    var files: [String]
    var errorMessage: String?
    var logPath: String
    var warnings: Int
    var errors: Int
    /// The disc's fingerprint (see "already archived"), or nil.
    var fingerprint: String?
}

/// Manifest handed to post-processing scripts (BROMELIA_MANIFEST).
struct JobManifest: Codable, Sendable {
    struct Title: Codable, Sendable {
        var index: Int
        var name: String
        var duration: String
        var chapters: Int
        var sourceTitleId: Int?
        var file: String?
    }

    struct File: Codable, Sendable {
        var path: String
        var size: Int64
        var sha256: String
    }

    var jobId: String
    var status: String
    var mode: String
    var driveName: String
    var driveId: String
    var devicePath: String
    var source: String
    var discName: String
    var discType: String
    var outputDirectory: String
    var files: [String]
    var titles: [Title]
    var name: String = ""
    var kind: String = ""
    var format: String = ""
    var formatCode: String = ""
    var encrypted: Bool = false
    var season: Int?
    var discNumber: Int?
    var episodes: [ArchiveRecord.Episode] = []
    var checksumFile: String?
    var checksums: [File] = []
    var startedAt: Date?
    var finishedAt: Date?
    var error: String?
}

/// Post-processing steps to run after a job has finished (and the disc is out), in the background queue.
struct BackgroundWork: Sendable {
    var jobId: UUID
    var title: String
    var steps: [PostProcessStep]
    var context: PostProcessor.Context
    var logFile: URL
}
