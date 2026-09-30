import Foundation

// The configuration model is shared (as JSON, see docs/configuration.md) with the Windows and
// Linux front-ends. Every type decodes leniently: missing keys fall back to defaults so that
// configuration files written by older or newer versions keep loading.

extension KeyedDecodingContainer {
    func value<T: Decodable>(_ key: K, _ fallback: @autoclosure () -> T) -> T {
        (try? decodeIfPresent(T.self, forKey: key)) ?? fallback()
    }
}

// MARK: - Title selection

struct TitleSelection: Codable, Hashable, Sendable {
    enum Strategy: String, Codable, CaseIterable, Sendable, Identifiable {
        case all, longest, indices, manual
        var id: String { rawValue }
        var label: String {
            switch self {
            case .all: return "All titles"
            case .longest: return "Longest title(s)"
            case .indices: return "Titles matching an index pattern"
            case .manual: return "Choose manually"
            }
        }
    }

    enum IndexBase: String, Codable, CaseIterable, Sendable, Identifiable {
        case makemkv, source
        var id: String { rawValue }
        var label: String { self == .makemkv ? "MakeMKV title number (0-based)" : "Source title ID (disc playlist / VTS)" }
    }

    var strategy: Strategy = .all
    var longestCount: Int = 1
    var indexPattern: String = ""
    var indexBase: IndexBase = .makemkv
    var minDurationSeconds: Int = 0
    var maxDurationSeconds: Int = 0
    var minChapters: Int = 0
    var maxChapters: Int = 0
    var minSizeMB: Int = 0
    var maxSizeMB: Int = 0
    var includePattern: String = ""
    var excludePattern: String = ""
    var skipDuplicates: Bool = true
    var skipAlternateAngles: Bool = false
    var maxTitles: Int = 0

    init() {}

    enum CodingKeys: String, CodingKey {
        case strategy, longestCount, indexPattern, indexBase, minDurationSeconds, maxDurationSeconds, minChapters,
             maxChapters, minSizeMB, maxSizeMB, includePattern, excludePattern, skipDuplicates, skipAlternateAngles, maxTitles
    }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        let d = TitleSelection()
        strategy = c.value(.strategy, d.strategy)
        longestCount = c.value(.longestCount, d.longestCount)
        indexPattern = c.value(.indexPattern, d.indexPattern)
        indexBase = c.value(.indexBase, d.indexBase)
        minDurationSeconds = c.value(.minDurationSeconds, d.minDurationSeconds)
        maxDurationSeconds = c.value(.maxDurationSeconds, d.maxDurationSeconds)
        minChapters = c.value(.minChapters, d.minChapters)
        maxChapters = c.value(.maxChapters, d.maxChapters)
        minSizeMB = c.value(.minSizeMB, d.minSizeMB)
        maxSizeMB = c.value(.maxSizeMB, d.maxSizeMB)
        includePattern = c.value(.includePattern, d.includePattern)
        excludePattern = c.value(.excludePattern, d.excludePattern)
        skipDuplicates = c.value(.skipDuplicates, d.skipDuplicates)
        skipAlternateAngles = c.value(.skipAlternateAngles, d.skipAlternateAngles)
        maxTitles = c.value(.maxTitles, d.maxTitles)
    }
}

// MARK: - Rip

enum RipMode: String, Codable, CaseIterable, Sendable, Identifiable {
    /// `audioCD` and `dataImage` are for discs without a DVD / Blu-ray structure (see OtherDiscsConfig).
    case mkv, backup, backupDecrypted, backupThenMkv, infoOnly, audioCD, dataImage
    var id: String { rawValue }
    /// The modes a drive can be set to (the others are chosen from the kind of disc).
    static let videoModes: [RipMode] = [.mkv, .backup, .backupDecrypted, .backupThenMkv, .infoOnly]
    var label: String {
        switch self {
        case .mkv: return "Rip titles to MKV"
        case .backup: return "Backup (encrypted, 1:1)"
        case .backupDecrypted: return "Backup (decrypted)"
        case .backupThenMkv: return "Decrypted backup, then MKV from backup"
        case .infoOnly: return "Scan only (save disc information)"
        case .audioCD: return "Rip audio CD"
        case .dataImage: return "Image data disc (ISO)"
        }
    }
    var shortLabel: String {
        switch self {
        case .mkv: return "MKV"
        case .backup: return "Backup"
        case .backupDecrypted: return "Decrypted backup"
        case .backupThenMkv: return "Backup + MKV"
        case .infoOnly: return "Scan"
        case .audioCD: return "Audio CD"
        case .dataImage: return "Disc image"
        }
    }
    var makesBackup: Bool { self == .backup || self == .backupDecrypted || self == .backupThenMkv }
    var makesMKV: Bool { self == .mkv || self == .backupThenMkv }
}

enum BackupFormat: String, Codable, CaseIterable, Sendable, Identifiable {
    case folder, iso
    var id: String { rawValue }
    var label: String { self == .folder ? "Folder (BDMV / VIDEO_TS)" : "ISO image" }
}

struct RipConfig: Codable, Hashable, Sendable {
    var mode: RipMode = .mkv
    var backupFormat: BackupFormat = .folder
    var keepBackupAfterMKV: Bool = true
    var titleSelection = TitleSelection()
    /// `--minlength` override in seconds. nil = use the drive's MakeMKV setting.
    var minLengthSeconds: Int? = nil
    /// `--cache` in MB. nil = MakeMKV default.
    var cacheMB: Int? = nil
    /// `--directio`. nil = MakeMKV default.
    var directIO: Bool? = nil
    /// Additional raw switches appended before the command (advanced).
    var extraArguments: String = ""
    var writeDiscInfoJSON: Bool = false
    /// Modes by disc format ("dvd", "bluray", "uhd") that replace `mode` for automatic and quick rips.
    var formatModes: [String: RipMode] = [:]

    init() {}

    /// The mode for a disc of `format` (automatic and quick rips).
    func mode(for format: DiscFormat?) -> RipMode {
        guard let format, let key = RipConfig.formatKey(format) else { return mode }
        return formatModes[key] ?? mode
    }

    static func formatKey(_ f: DiscFormat) -> String? {
        switch f {
        case .dvd: return "dvd"
        case .bluray: return "bluray"
        case .uhd: return "uhd"
        default: return nil
        }
    }

    enum CodingKeys: String, CodingKey {
        case mode, backupFormat, keepBackupAfterMKV, titleSelection, minLengthSeconds, cacheMB, directIO, extraArguments, writeDiscInfoJSON, formatModes
    }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        let d = RipConfig()
        mode = c.value(.mode, d.mode)
        backupFormat = c.value(.backupFormat, d.backupFormat)
        keepBackupAfterMKV = c.value(.keepBackupAfterMKV, d.keepBackupAfterMKV)
        titleSelection = c.value(.titleSelection, d.titleSelection)
        minLengthSeconds = try? c.decodeIfPresent(Int.self, forKey: .minLengthSeconds)
        cacheMB = try? c.decodeIfPresent(Int.self, forKey: .cacheMB)
        directIO = try? c.decodeIfPresent(Bool.self, forKey: .directIO)
        extraArguments = c.value(.extraArguments, d.extraArguments)
        writeDiscInfoJSON = c.value(.writeDiscInfoJSON, d.writeDiscInfoJSON)
        formatModes = c.value(.formatModes, d.formatModes)
    }
}

// MARK: - Output

enum ConflictPolicy: String, Codable, CaseIterable, Sendable, Identifiable {
    case uniqueSuffix, overwrite, skip
    var id: String { rawValue }
    var label: String {
        switch self {
        case .uniqueSuffix: return "Add a number (Disc (2))"
        case .overwrite: return "Reuse the existing folder"
        case .skip: return "Skip the job"
        }
    }
}

/// How output is named: with the folder and file name templates, or the way media servers expect it.
enum LibraryLayout: String, Codable, CaseIterable, Sendable, Identifiable {
    /// The folder and file name templates.
    case templates
    /// Plex / Jellyfin / Emby: `Movies/Name (Year)/Name (Year).mkv`,
    /// `TV Shows/Name (Year)/Season 02/Name (Year) - S02E05.mkv`, other titles in `Other/`.
    case mediaServer
    var id: String { rawValue }
    var label: String { self == .templates ? "Folder and file name templates" : "Plex / Jellyfin / Emby library" }
}

struct OutputConfig: Codable, Hashable, Sendable {
    /// `{name} - {episode} - {episodeTitle} - {discLabel} - {rip} - {track} - {format}`, leaving out the parts that don't apply.
    static let defaultFileNameTemplate = "{name}{episode? - {episode}}{episodeTitle? - {episodeTitle}}{discLabel? - {discLabel}} - {rip}{track? - {track}} - {format}"
    static let defaultFolderTemplate = "{name}{discLabel? - {discLabel}}"
    /// Templates of configuration version 1, replaced by the defaults above when a configuration is upgraded.
    static let legacyFolderTemplate = "{disc}"

    /// Empty = use the global output root.
    var rootOverride: String = ""
    var folderTemplate: String = OutputConfig.defaultFolderTemplate
    /// Used for MKV files and backups (without extension). Empty = keep MakeMKV's own file names.
    var fileNameTemplate: String = OutputConfig.defaultFileNameTemplate
    var backupSubfolder: String = "backup"
    var conflictPolicy: ConflictPolicy = .uniqueSuffix
    var layout: LibraryLayout = .templates

    init() {}

    enum CodingKeys: String, CodingKey { case rootOverride, folderTemplate, fileNameTemplate, backupSubfolder, conflictPolicy, layout }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        let d = OutputConfig()
        rootOverride = c.value(.rootOverride, d.rootOverride)
        folderTemplate = c.value(.folderTemplate, d.folderTemplate)
        fileNameTemplate = c.value(.fileNameTemplate, d.fileNameTemplate)
        backupSubfolder = c.value(.backupSubfolder, d.backupSubfolder)
        conflictPolicy = c.value(.conflictPolicy, d.conflictPolicy)
        layout = c.value(.layout, d.layout)
    }
}

// MARK: - Automation

struct AutomationConfig: Codable, Hashable, Sendable {
    var autoRipOnInsert: Bool = false
    /// Seconds to wait before an automatic rip starts (gives time to cancel).
    var autoRipDelaySeconds: Int = 10
    var ejectWhenDone: Bool = true
    var ejectOnFailure: Bool = false
    var notify: Bool = true
    var playSound: Bool = true
    /// Before an automatic rip, wait up to this long for the system to mount the disc. 0 = don't wait.
    var waitForMountSeconds: Int = 30
    /// What an automatic rip does with a disc that was archived before (manual rips only warn).
    var alreadyArchived: AlreadyArchived = .skip

    init() {}

    enum CodingKeys: String, CodingKey {
        case autoRipOnInsert, autoRipDelaySeconds, ejectWhenDone, ejectOnFailure, notify, playSound, waitForMountSeconds, alreadyArchived
    }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        let d = AutomationConfig()
        autoRipOnInsert = c.value(.autoRipOnInsert, d.autoRipOnInsert)
        autoRipDelaySeconds = c.value(.autoRipDelaySeconds, d.autoRipDelaySeconds)
        ejectWhenDone = c.value(.ejectWhenDone, d.ejectWhenDone)
        ejectOnFailure = c.value(.ejectOnFailure, d.ejectOnFailure)
        notify = c.value(.notify, d.notify)
        playSound = c.value(.playSound, d.playSound)
        waitForMountSeconds = c.value(.waitForMountSeconds, d.waitForMountSeconds)
        alreadyArchived = c.value(.alreadyArchived, d.alreadyArchived)
    }
}

/// What an automatic rip does with a disc found in the history or the output folder (same disc fingerprint).
enum AlreadyArchived: String, Codable, CaseIterable, Sendable, Identifiable {
    case skip, ask, ripAgain
    var id: String { rawValue }
    var label: String {
        switch self {
        case .skip: return "Skip it (eject when done)"
        case .ask: return "Stop and ask (leave it in the drive)"
        case .ripAgain: return "Rip it again"
        }
    }
}

// MARK: - Other discs

/// What to do with discs that aren't DVDs or Blu-rays, when they are ripped automatically or with “Rip”.
struct OtherDiscsConfig: Codable, Hashable, Sendable {
    /// Rip audio CDs with `audioCommand` (cyanrip or abcde look up the album in MusicBrainz).
    var ripAudioCDs: Bool = true
    /// Save data discs (CD-ROM, DVD-ROM, BD-ROM without video) as ISO images.
    var imageDataDiscs: Bool = true
    /// Command for audio CDs, run in the output folder. `{device}` is the drive. Empty = cyanrip, else abcde.
    var audioCommand: String = ""

    init() {}

    enum CodingKeys: String, CodingKey { case ripAudioCDs, imageDataDiscs, audioCommand }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        let d = OtherDiscsConfig()
        ripAudioCDs = c.value(.ripAudioCDs, d.ripAudioCDs)
        imageDataDiscs = c.value(.imageDataDiscs, d.imageDataDiscs)
        audioCommand = c.value(.audioCommand, d.audioCommand)
    }
}

// MARK: - Archiving

struct ArchiveConfig: Codable, Hashable, Sendable {
    /// Write SHA256SUMS (sha256sum / shasum -a 256 format) for every produced file into the output folder.
    var checksums: Bool = true
    /// Write bromelia.json (disc identity, titles, files with sizes and hashes) and the job log into the output folder.
    var archiveRecord: Bool = true
    /// Check every ripped MKV against the disc listing (duration, tracks, chapters) with mkvmerge.
    var verifyRips: Bool = true

    init() {}

    enum CodingKeys: String, CodingKey { case checksums, archiveRecord, verifyRips }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        let d = ArchiveConfig()
        checksums = c.value(.checksums, d.checksums)
        archiveRecord = c.value(.archiveRecord, d.archiveRecord)
        verifyRips = c.value(.verifyRips, d.verifyRips)
    }
}

// MARK: - Episodes

struct EpisodeConfig: Codable, Hashable, Sendable {
    /// Split DVD "play all" titles of TV shows into one file per episode, using the disc's menu navigation.
    var splitPlayAll: Bool = true
    /// Keep the unsplit title next to the episode files.
    var keepPlayAll: Bool = true
    /// Read episode numbers from the disc's episode menus (needs ffmpeg and tesseract).
    var readMenuNumbers: Bool = true

    init() {}

    enum CodingKeys: String, CodingKey { case splitPlayAll, keepPlayAll, readMenuNumbers }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        let d = EpisodeConfig()
        splitPlayAll = c.value(.splitPlayAll, d.splitPlayAll)
        keepPlayAll = c.value(.keepPlayAll, d.keepPlayAll)
        readMenuNumbers = c.value(.readMenuNumbers, d.readMenuNumbers)
    }
}

// MARK: - Post-processing

enum RunCondition: String, Codable, CaseIterable, Sendable, Identifiable {
    case success, failure, always
    var id: String { rawValue }
    var label: String {
        switch self {
        case .success: return "When the rip succeeds"
        case .failure: return "When the rip fails"
        case .always: return "Always"
        }
    }
}

/// What a post-processing step runs: a program or script, or HandBrakeCLI with a preset.
enum StepKind: String, Codable, CaseIterable, Sendable, Identifiable {
    case command, handbrake
    var id: String { rawValue }
    var label: String { self == .command ? "Program or script" : "Transcode with HandBrake" }
}

struct PostProcessStep: Codable, Hashable, Sendable, Identifiable {
    /// HandBrake's preset for new HandBrake steps: HEVC in MKV, 1080p.
    static let defaultPreset = "H.265 MKV 1080p30"
    static let defaultEncodePath = "{outputDir}/Encoded/{stem}.mkv"

    var id = UUID()
    var name: String = "Post-processing script"
    var enabled: Bool = true
    /// Script or program to run.
    var executable: String = ""
    /// Optional interpreter (e.g. /bin/zsh, /usr/bin/python3). Empty = run `executable` directly.
    var interpreter: String = ""
    /// Argument template, split like a shell command line. Supports the same {tokens} as file names.
    var arguments: String = "{outputDir}"
    /// Working directory template. Empty = the job's output folder.
    var workingDirectory: String = ""
    var runOn: RunCondition = .success
    /// Run once per produced file instead of once per job.
    var perFile: Bool = false
    var timeoutSeconds: Int = 0
    var environment: [String: String] = [:]
    /// Mark the job as failed when this step exits with a non-zero status.
    var failJobOnError: Bool = false
    /// Run only for discs whose movie / show name or disc label matches this regular expression (empty = all).
    var matchName: String = ""
    /// Run only for these format codes (DVD, DVDe, BR, BRe, 4K, 4Ke; `BR*` = any code starting with BR). Empty = all.
    var matchFormats: [String] = []
    /// Run after the job has finished and the disc is out, in a queue of its own (encoding, uploads), so the
    /// drive is free for the next disc. Such steps can't fail the job.
    var background: Bool = false
    var kind: StepKind = .command
    /// HandBrake steps: a preset name (HandBrakeCLI --preset-list), optionally from a preset file exported from HandBrake.
    var preset: String = PostProcessStep.defaultPreset
    var presetFile: String = ""
    /// HandBrake steps: where each encode goes (a template; the extension picks the container).
    var outputPath: String = PostProcessStep.defaultEncodePath
    /// HandBrake steps: more HandBrakeCLI arguments, split like a command line.
    var extraArguments: String = ""

    init() {}

    /// A HandBrake step that encodes every MKV in the background queue.
    static func handbrake() -> PostProcessStep {
        var s = PostProcessStep()
        s.name = "Transcode with HandBrake"
        s.kind = .handbrake
        s.executable = ""
        s.arguments = ""
        s.perFile = true
        s.background = true
        return s
    }

    enum CodingKeys: String, CodingKey {
        case id, name, enabled, executable, interpreter, arguments, workingDirectory, runOn, perFile, timeoutSeconds, environment, failJobOnError,
             matchName, matchFormats, background, kind, preset, presetFile, outputPath, extraArguments
    }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        let d = PostProcessStep()
        id = c.value(.id, UUID())
        name = c.value(.name, d.name)
        enabled = c.value(.enabled, d.enabled)
        executable = c.value(.executable, d.executable)
        interpreter = c.value(.interpreter, d.interpreter)
        arguments = c.value(.arguments, d.arguments)
        workingDirectory = c.value(.workingDirectory, d.workingDirectory)
        runOn = c.value(.runOn, d.runOn)
        perFile = c.value(.perFile, d.perFile)
        timeoutSeconds = c.value(.timeoutSeconds, d.timeoutSeconds)
        environment = c.value(.environment, d.environment)
        failJobOnError = c.value(.failJobOnError, d.failJobOnError)
        matchName = c.value(.matchName, d.matchName)
        matchFormats = c.value(.matchFormats, d.matchFormats)
        background = c.value(.background, d.background)
        kind = c.value(.kind, d.kind)
        preset = c.value(.preset, d.preset)
        presetFile = c.value(.presetFile, d.presetFile)
        outputPath = c.value(.outputPath, d.outputPath)
        extraArguments = c.value(.extraArguments, d.extraArguments)
    }
}

// MARK: - Profile

enum LPCMOutput: String, Codable, CaseIterable, Sendable, Identifiable {
    case copy, lpcm, wavex, flacBest = "flac-best", flacFast = "flac-fast"
    var id: String { rawValue }
    var label: String {
        switch self {
        case .copy: return "Copy as is"
        case .lpcm: return "Raw LPCM"
        case .wavex: return "LPCM in WAV container"
        case .flacBest: return "FLAC (best compression)"
        case .flacFast: return "FLAC (fast compression)"
        }
    }
}

struct GeneratedProfile: Codable, Hashable, Sendable {
    var name: String = "Bromelia"
    /// Empty = MakeMKV's default rule.
    var selectionRule: String = ""
    var setFirstAudioTrackAsDefault: Bool = true
    var setFirstSubtitleTrackAsDefault: Bool = true
    var setFirstForcedSubtitleTrackAsDefault: Bool = true
    var ignoreForcedSubtitlesFlag: Bool = true
    var useISO639Type2T: Bool = false
    var insertFirstChapter00IfMissing: Bool = true
    var lpcmStereo: LPCMOutput = .lpcm
    var lpcmMultichannel: LPCMOutput = .flacBest

    init() {}

    enum CodingKeys: String, CodingKey {
        case name, selectionRule, setFirstAudioTrackAsDefault, setFirstSubtitleTrackAsDefault, setFirstForcedSubtitleTrackAsDefault,
             ignoreForcedSubtitlesFlag, useISO639Type2T, insertFirstChapter00IfMissing, lpcmStereo, lpcmMultichannel
    }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        let d = GeneratedProfile()
        name = c.value(.name, d.name)
        selectionRule = c.value(.selectionRule, d.selectionRule)
        setFirstAudioTrackAsDefault = c.value(.setFirstAudioTrackAsDefault, d.setFirstAudioTrackAsDefault)
        setFirstSubtitleTrackAsDefault = c.value(.setFirstSubtitleTrackAsDefault, d.setFirstSubtitleTrackAsDefault)
        setFirstForcedSubtitleTrackAsDefault = c.value(.setFirstForcedSubtitleTrackAsDefault, d.setFirstForcedSubtitleTrackAsDefault)
        ignoreForcedSubtitlesFlag = c.value(.ignoreForcedSubtitlesFlag, d.ignoreForcedSubtitlesFlag)
        useISO639Type2T = c.value(.useISO639Type2T, d.useISO639Type2T)
        insertFirstChapter00IfMissing = c.value(.insertFirstChapter00IfMissing, d.insertFirstChapter00IfMissing)
        lpcmStereo = c.value(.lpcmStereo, d.lpcmStereo)
        lpcmMultichannel = c.value(.lpcmMultichannel, d.lpcmMultichannel)
    }
}

enum ProfileMode: String, Codable, CaseIterable, Sendable, Identifiable {
    case makemkvDefault, generated, customFile
    var id: String { rawValue }
    var label: String {
        switch self {
        case .makemkvDefault: return "MakeMKV default profile"
        case .generated: return "Bromelia profile (edit below)"
        case .customFile: return "Custom profile file (.mmcp.xml)"
        }
    }
}

struct ProfileConfig: Codable, Hashable, Sendable {
    var mode: ProfileMode = .makemkvDefault
    var customPath: String = ""
    var generated = GeneratedProfile()

    init() {}

    enum CodingKeys: String, CodingKey { case mode, customPath, generated }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        mode = c.value(.mode, .makemkvDefault)
        customPath = c.value(.customPath, "")
        generated = c.value(.generated, GeneratedProfile())
    }
}

// MARK: - Drive

struct DriveMatch: Codable, Hashable, Sendable {
    /// Drive identification string reported by MakeMKV (model, firmware and usually serial).
    var driveName: String = ""
    /// OS device path, e.g. /dev/rdisk4. Used when `driveName` is empty or ambiguous.
    var devicePath: String = ""

    init(driveName: String = "", devicePath: String = "") {
        self.driveName = driveName
        self.devicePath = devicePath
    }

    enum CodingKeys: String, CodingKey { case driveName, devicePath }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        driveName = c.value(.driveName, "")
        devicePath = c.value(.devicePath, "")
    }

    func matches(_ e: DriveScanEntry) -> Bool {
        let name = driveName.trimmingCharacters(in: .whitespaces)
        if !name.isEmpty {
            return DriveMatch.normalize(name) == DriveMatch.normalize(e.driveName)
        }
        return !devicePath.isEmpty && devicePath == e.devicePath
    }

    static func normalize(_ s: String) -> String {
        s.split(whereSeparator: { $0 == " " || $0 == "\t" }).joined(separator: " ").lowercased()
    }
}

struct DriveConfig: Codable, Hashable, Sendable, Identifiable {
    var id = UUID()
    var name: String = "Drive"
    var enabled: Bool = true
    var match = DriveMatch()
    /// MakeMKV settings.conf overrides for this drive. A missing key inherits the global value.
    var settings: [String: String] = [:]
    var profile = ProfileConfig()
    var rip = RipConfig()
    var output = OutputConfig()
    var automation = AutomationConfig()
    var archive = ArchiveConfig()
    var episodes = EpisodeConfig()
    var other = OtherDiscsConfig()
    var postProcess: [PostProcessStep] = []

    init() {}

    enum CodingKeys: String, CodingKey { case id, name, enabled, match, settings, profile, rip, output, automation, archive, episodes, other, postProcess }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        let d = DriveConfig()
        id = c.value(.id, UUID())
        name = c.value(.name, d.name)
        enabled = c.value(.enabled, d.enabled)
        match = c.value(.match, d.match)
        settings = c.value(.settings, d.settings)
        profile = c.value(.profile, d.profile)
        rip = c.value(.rip, d.rip)
        output = c.value(.output, d.output)
        automation = c.value(.automation, d.automation)
        archive = c.value(.archive, d.archive)
        episodes = c.value(.episodes, d.episodes)
        other = c.value(.other, d.other)
        postProcess = c.value(.postProcess, d.postProcess)
    }

    /// Version 1 defaults kept MakeMKV's file names and named folders after the disc label; version 2
    /// names everything `{name} - … - {format}`. Only untouched defaults are replaced.
    mutating func upgradeNaming(from version: Int) {
        guard version < 2 else { return }
        if output.fileNameTemplate.trimmingCharacters(in: .whitespaces).isEmpty { output.fileNameTemplate = OutputConfig.defaultFileNameTemplate }
        if output.folderTemplate == OutputConfig.legacyFolderTemplate { output.folderTemplate = OutputConfig.defaultFolderTemplate }
    }

    /// Settings for archiving everything on a disc: a decrypted backup (every file, every title, menus) kept
    /// next to MKV files of every title with every track, the full disc listing, and all checks on.
    /// MakeMKV's minimum title length still applies to the MKV files; the backup contains every title.
    mutating func applyArchiveEverything() {
        rip.mode = .backupThenMkv
        rip.backupFormat = .folder
        rip.keepBackupAfterMKV = true
        rip.titleSelection = TitleSelection()
        rip.writeDiscInfoJSON = true
        profile.mode = .generated
        profile.generated.selectionRule = "+sel:all"
        archive.checksums = true
        archive.archiveRecord = true
        archive.verifyRips = true
        episodes.keepPlayAll = true
    }

    /// Returns a copy that keeps identity (id, name, match, enabled) but takes everything else from `other`.
    func applyingBody(of other: DriveConfig) -> DriveConfig {
        var c = other
        c.id = id
        c.name = name
        c.match = match
        c.enabled = enabled
        return c
    }
}

struct DrivePreset: Codable, Hashable, Sendable, Identifiable {
    var id = UUID()
    var name: String
    var config: DriveConfig
}

// MARK: - App

struct AppConfig: Codable, Hashable, Sendable {
    static let currentVersion = 2

    var version: Int = AppConfig.currentVersion
    /// Empty = autodetect.
    var makemkvconPath: String = ""
    /// Empty = autodetect. Needed for per-track selection and track renaming.
    var mkvmergePath: String = ""
    var outputRoot: String = "~/Movies/Bromelia"
    var pollIntervalSeconds: Int = 10
    var pollWhileRipping: Bool = false
    /// 0 = unlimited (one job per drive at a time is always enforced).
    var maxConcurrentJobs: Int = 0
    var registrationKey: String = ""
    /// MakeMKV settings applied to every drive (the equivalent of MakeMKV's Preferences).
    var globalSettings: [String: String] = [:]
    /// Template used for drives without their own configuration and for ISO / folder sources.
    var defaultDrive: DriveConfig = {
        var d = DriveConfig()
        d.name = "Default"
        return d
    }()
    var drives: [DriveConfig] = []
    var presets: [DrivePreset] = []
    /// Post-processing steps for every drive, usually limited to certain titles or formats with
    /// `matchName` / `matchFormats` ("plugins"). They run after the drive's own steps.
    var plugins: [PostProcessStep] = []
    var historyLimit: Int = 500
    /// A rip or backup that prints nothing for this long is stopped and fails (a stuck drive). 0 = never.
    var stallTimeoutMinutes: Int = 30
    /// Keep the computer from going to sleep while jobs run.
    var preventSleep: Bool = true
    /// Online lookup of the movie / show (canonical title, year, ids).
    var metadata = MetadataConfig()
    /// Where to send notifications of finished jobs (webhooks, ntfy, Discord, Slack, any Apprise URL).
    var notifications: [NotificationTarget] = []
    /// Download and register MakeMKV's current beta key at startup and when the key has expired.
    var autoUpdateBetaKey: Bool = false
    /// How many background post-processing steps (encoding, uploads) run at the same time.
    var backgroundJobs: Int = 1
    /// Web page for watching and controlling Bromelia from a browser.
    var webUI = WebUIConfig()
    /// Verifying the output folder's archives again on a schedule.
    var archiveCheck = ArchiveCheckConfig()

    init() {}

    enum CodingKeys: String, CodingKey {
        case version, makemkvconPath, mkvmergePath, outputRoot, pollIntervalSeconds, pollWhileRipping, maxConcurrentJobs,
             registrationKey, globalSettings, defaultDrive, drives, presets, plugins, historyLimit, stallTimeoutMinutes, preventSleep,
             metadata, notifications, autoUpdateBetaKey, backgroundJobs, webUI, archiveCheck
    }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        let d = AppConfig()
        version = c.value(.version, d.version)
        makemkvconPath = c.value(.makemkvconPath, d.makemkvconPath)
        mkvmergePath = c.value(.mkvmergePath, d.mkvmergePath)
        outputRoot = c.value(.outputRoot, d.outputRoot)
        pollIntervalSeconds = c.value(.pollIntervalSeconds, d.pollIntervalSeconds)
        pollWhileRipping = c.value(.pollWhileRipping, d.pollWhileRipping)
        maxConcurrentJobs = c.value(.maxConcurrentJobs, d.maxConcurrentJobs)
        registrationKey = c.value(.registrationKey, d.registrationKey)
        globalSettings = c.value(.globalSettings, d.globalSettings)
        defaultDrive = c.value(.defaultDrive, d.defaultDrive)
        drives = c.value(.drives, d.drives)
        presets = c.value(.presets, d.presets)
        plugins = c.value(.plugins, d.plugins)
        historyLimit = c.value(.historyLimit, d.historyLimit)
        stallTimeoutMinutes = c.value(.stallTimeoutMinutes, d.stallTimeoutMinutes)
        preventSleep = c.value(.preventSleep, d.preventSleep)
        metadata = c.value(.metadata, d.metadata)
        notifications = c.value(.notifications, d.notifications)
        autoUpdateBetaKey = c.value(.autoUpdateBetaKey, d.autoUpdateBetaKey)
        backgroundJobs = c.value(.backgroundJobs, d.backgroundJobs)
        webUI = c.value(.webUI, d.webUI)
        archiveCheck = c.value(.archiveCheck, d.archiveCheck)
        // Files without a version are treated as version 1.
        let loaded = (try? c.decodeIfPresent(Int.self, forKey: .version)) ?? 1
        if loaded < 2 {
            defaultDrive.upgradeNaming(from: loaded)
            for i in drives.indices { drives[i].upgradeNaming(from: loaded) }
            for i in presets.indices { presets[i].config.upgradeNaming(from: loaded) }
        }
        version = AppConfig.currentVersion
    }

    func driveConfig(for entry: DriveScanEntry) -> DriveConfig? {
        drives.first { $0.enabled && $0.match.matches(entry) }
            ?? drives.first { $0.match.matches(entry) }
    }

    func driveConfig(id: UUID) -> DriveConfig? {
        if defaultDrive.id == id { return defaultDrive }
        return drives.first { $0.id == id }
    }

    /// Effective MakeMKV settings for a drive: global settings overlaid with the drive's overrides.
    func effectiveSettings(for drive: DriveConfig) -> [String: String] {
        var s = globalSettings
        for (k, v) in drive.settings { s[k] = v }
        if !registrationKey.isEmpty { s["app_Key"] = registrationKey }
        return s
    }

    func outputRoot(for drive: DriveConfig) -> String {
        let r = drive.output.rootOverride.trimmingCharacters(in: .whitespaces)
        return r.isEmpty ? outputRoot : r
    }
}

// MARK: - Online services

enum MetadataProvider: String, Codable, CaseIterable, Sendable, Identifiable {
    case none, tmdb, omdb
    var id: String { rawValue }
    var label: String {
        switch self {
        case .none: return "Off"
        case .tmdb: return "The Movie Database (TMDb)"
        case .omdb: return "OMDb (IMDb data)"
        }
    }
}

struct MetadataConfig: Codable, Hashable, Sendable {
    var provider: MetadataProvider = .none
    /// TMDb: API key (v3) or read access token; OMDb: API key.
    var apiKey: String = ""
    /// TMDb language for titles, e.g. en-US.
    var language: String = "en-US"
    /// TV shows: look up the titles of the disc's episodes ({episodeTitle}).
    var episodeTitles: Bool = true
    /// Media server layout: write Kodi / Jellyfin / Emby .nfo files and the poster next to the files.
    var nfo: Bool = true

    init() {}

    enum CodingKeys: String, CodingKey { case provider, apiKey, language, episodeTitles, nfo }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        let d = MetadataConfig()
        provider = c.value(.provider, d.provider)
        apiKey = c.value(.apiKey, d.apiKey)
        language = c.value(.language, d.language)
        episodeTitles = c.value(.episodeTitles, d.episodeTitles)
        nfo = c.value(.nfo, d.nfo)
    }
}

/// A place to send job notifications. `url` is an http(s) webhook (Discord and Slack webhooks are recognised),
/// `ntfy://topic` / `ntfys://host/topic`, or any other Apprise URL (sent with the `apprise` command).
struct NotificationTarget: Codable, Hashable, Sendable, Identifiable {
    var id = UUID()
    var url: String = ""
    var enabled: Bool = true
    /// Only for jobs that didn't succeed (failed, cancelled, read errors).
    var onlyProblems: Bool = false

    init() {}

    enum CodingKeys: String, CodingKey { case id, url, enabled, onlyProblems }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        let d = NotificationTarget()
        id = c.value(.id, UUID())
        url = c.value(.url, d.url)
        enabled = c.value(.enabled, d.enabled)
        onlyProblems = c.value(.onlyProblems, d.onlyProblems)
    }
}

struct ArchiveCheckConfig: Codable, Hashable, Sendable {
    /// Verify every archive folder under the output root again every this many days (SHA256SUMS). 0 = never.
    var intervalDays: Int = 0

    init() {}

    enum CodingKeys: String, CodingKey { case intervalDays }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        intervalDays = max(0, c.value(.intervalDays, 0))
    }
}

struct WebUIConfig: Codable, Hashable, Sendable {
    var enabled: Bool = false
    /// 127.0.0.1 = this computer only; 0.0.0.0 = the network (set a token).
    var address: String = "127.0.0.1"
    var port: Int = 51280
    /// Required for everything when set (header `Authorization: Bearer <token>` or `?token=`).
    var token: String = ""

    init() {}

    enum CodingKeys: String, CodingKey { case enabled, address, port, token }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        let d = WebUIConfig()
        enabled = c.value(.enabled, d.enabled)
        address = c.value(.address, d.address)
        port = c.value(.port, d.port)
        token = c.value(.token, d.token)
    }
}
