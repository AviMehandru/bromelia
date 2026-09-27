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
    case mkv, backup, backupDecrypted, backupThenMkv, infoOnly
    var id: String { rawValue }
    var label: String {
        switch self {
        case .mkv: return "Rip titles to MKV"
        case .backup: return "Backup (encrypted, 1:1)"
        case .backupDecrypted: return "Backup (decrypted)"
        case .backupThenMkv: return "Decrypted backup, then MKV from backup"
        case .infoOnly: return "Scan only (save disc information)"
        }
    }
    var shortLabel: String {
        switch self {
        case .mkv: return "MKV"
        case .backup: return "Backup"
        case .backupDecrypted: return "Decrypted backup"
        case .backupThenMkv: return "Backup + MKV"
        case .infoOnly: return "Scan"
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

    init() {}

    enum CodingKeys: String, CodingKey {
        case mode, backupFormat, keepBackupAfterMKV, titleSelection, minLengthSeconds, cacheMB, directIO, extraArguments, writeDiscInfoJSON
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

struct OutputConfig: Codable, Hashable, Sendable {
    /// Empty = use the global output root.
    var rootOverride: String = ""
    var folderTemplate: String = "{disc}"
    /// Empty = keep MakeMKV's own file names.
    var fileNameTemplate: String = ""
    var backupSubfolder: String = "backup"
    var conflictPolicy: ConflictPolicy = .uniqueSuffix

    init() {}

    enum CodingKeys: String, CodingKey { case rootOverride, folderTemplate, fileNameTemplate, backupSubfolder, conflictPolicy }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        let d = OutputConfig()
        rootOverride = c.value(.rootOverride, d.rootOverride)
        folderTemplate = c.value(.folderTemplate, d.folderTemplate)
        fileNameTemplate = c.value(.fileNameTemplate, d.fileNameTemplate)
        backupSubfolder = c.value(.backupSubfolder, d.backupSubfolder)
        conflictPolicy = c.value(.conflictPolicy, d.conflictPolicy)
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

    init() {}

    enum CodingKeys: String, CodingKey { case autoRipOnInsert, autoRipDelaySeconds, ejectWhenDone, ejectOnFailure, notify, playSound }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        let d = AutomationConfig()
        autoRipOnInsert = c.value(.autoRipOnInsert, d.autoRipOnInsert)
        autoRipDelaySeconds = c.value(.autoRipDelaySeconds, d.autoRipDelaySeconds)
        ejectWhenDone = c.value(.ejectWhenDone, d.ejectWhenDone)
        ejectOnFailure = c.value(.ejectOnFailure, d.ejectOnFailure)
        notify = c.value(.notify, d.notify)
        playSound = c.value(.playSound, d.playSound)
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

struct PostProcessStep: Codable, Hashable, Sendable, Identifiable {
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

    init() {}

    enum CodingKeys: String, CodingKey {
        case id, name, enabled, executable, interpreter, arguments, workingDirectory, runOn, perFile, timeoutSeconds, environment, failJobOnError
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
    var postProcess: [PostProcessStep] = []

    init() {}

    enum CodingKeys: String, CodingKey { case id, name, enabled, match, settings, profile, rip, output, automation, postProcess }

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
        postProcess = c.value(.postProcess, d.postProcess)
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
    static let currentVersion = 1

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
    var historyLimit: Int = 500

    init() {}

    enum CodingKeys: String, CodingKey {
        case version, makemkvconPath, mkvmergePath, outputRoot, pollIntervalSeconds, pollWhileRipping, maxConcurrentJobs,
             registrationKey, globalSettings, defaultDrive, drives, presets, historyLimit
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
        historyLimit = c.value(.historyLimit, d.historyLimit)
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
