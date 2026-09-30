import Foundation
import Observation

/// The disc currently opened in the UI for one source (a drive, an ISO file or a folder):
/// its title listing and the user's title / track choices.
@MainActor
@Observable
final class DiscSession: Identifiable {
    let id: String
    var source: DiscSource
    /// Configuration used to open the disc (drive configuration or, for files, the chosen one).
    var configId: UUID
    var info: DiscInfo?
    var isLoading = false
    var loadError: String?
    var log: [LogEntry] = []
    var progress = 0.0
    var operation = ""
    var selectedTitles: Set<Int> = []
    /// Only titles whose tracks were changed by the user appear here.
    var trackSelections: [Int: Set<Int>] = [:]
    /// Output file names typed by the user (MakeMKV's expert-mode rename), keyed by title index.
    var titleNameOverrides: [Int: String] = [:]
    /// One-off output folder for the next rip from this disc. Empty = the configuration's folder.
    var outputFolderOverride = ""
    /// Movie / show name for file names. Empty = inferred from the disc.
    var mediaName = ""
    /// nil = decide automatically.
    var mediaKind: MediaKind?
    /// First episode number on this disc. nil = read from the menus, continued from the previous disc, or 1.
    var firstEpisode: Int?
    /// Release year, to find the right movie or show online. nil = none.
    var mediaYear: Int?
    /// The movie or show chosen online (a TMDb or IMDb id). "" = the best search result.
    var onlineId = ""
    /// What the online lookup found for this disc, best first.
    var lookupCandidates: [MediaMatch] = []
    var isLookingUp = false
    /// Why the lookup found nothing, or nil.
    var lookupMessage: String?
    @ObservationIgnored var lookupGeneration = 0
    /// File system flags from the drive scan.
    var discFlags: DiscFlags?
    /// What MakeMKV said about LibreDrive when the disc was opened (drives only).
    var libreDrive: LibreDriveState?
    @ObservationIgnored var runner: ProcessRunner?
    @ObservationIgnored private var nextLog = 0

    init(id: String, source: DiscSource, configId: UUID) {
        self.id = id
        self.source = source
        self.configId = configId
    }

    func appendLog(_ text: String, severity: RobotMessage.Severity) {
        log.append(LogEntry(id: nextLog, time: Date(), severity: severity, text: text))
        nextLog += 1
        if log.count > 5_000 { log.removeFirst(1_000) }
    }

    func reset() {
        runner?.cancel()
        runner = nil
        info = nil
        isLoading = false
        loadError = nil
        selectedTitles = []
        trackSelections = [:]
        titleNameOverrides = [:]
        mediaName = ""
        mediaKind = nil
        firstEpisode = nil
        mediaYear = nil
        onlineId = ""
        lookupCandidates = []
        isLookingUp = false
        lookupMessage = nil
        lookupGeneration += 1
        libreDrive = nil
        progress = 0
        operation = ""
    }

    /// Applies the configuration's title rules to preselect titles (like MakeMKV's default selection).
    func applyRule(_ rule: TitleSelection) {
        guard let info else { return }
        var r = rule
        if r.strategy == .manual { r.strategy = .all }
        selectedTitles = Set(TitleSelector.evaluate(info.titles, rule: r).selectedIndices)
        trackSelections = [:]
    }

    /// Tracks of a title are chosen by the profile's selection rule unless the user customises them.
    func hasCustomTracks(_ title: Int) -> Bool { trackSelections[title] != nil }

    func customizeTracks(_ title: Int) {
        guard let t = info?.title(at: title) else { return }
        trackSelections[title] = Set(t.tracks.map(\.index))
    }

    func resetTracks(_ title: Int) { trackSelections[title] = nil }

    func isTrackSelected(title: Int, track: Int) -> Bool {
        trackSelections[title]?.contains(track) ?? false
    }

    func setTrack(title: Int, track: Int, selected: Bool) {
        guard var set = trackSelections[title] else { return }
        if selected { set.insert(track) } else { set.remove(track) }
        trackSelections[title] = set
    }

    var selectedSizeBytes: Int64 {
        guard let info else { return 0 }
        return info.titles.filter { selectedTitles.contains($0.index) }.reduce(0) { $0 + $1.sizeBytes }
    }
}

/// LibreDrive as reported while opening a disc.
enum LibreDriveState: Equatable, Sendable {
    /// "Using LibreDrive mode (…)", with the details (version and drive id).
    case enabled(String)
    /// The disc needs a LibreDrive-compatible drive and this one isn't.
    case required
    /// MakeMKV didn't mention LibreDrive: the drive doesn't use it for this disc (fine for DVDs and most Blu-rays).
    case notInUse

    var label: String {
        switch self {
        case .enabled(let d): return "LibreDrive: enabled\(d.isEmpty ? "" : " (\(d))")"
        case .required: return "LibreDrive required: this drive can't decrypt this disc"
        case .notInUse: return "LibreDrive: not in use for this disc"
        }
    }
}
