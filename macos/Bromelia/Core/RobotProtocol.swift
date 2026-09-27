import Foundation

// MARK: - Constants from MakeMKV's apdefs.h

/// Attribute identifiers used in CINFO / TINFO / SINFO lines.
enum AttributeID: Int, CaseIterable, Sendable {
    case unknown = 0
    case type = 1
    case name = 2
    case langCode = 3
    case langName = 4
    case codecId = 5
    case codecShort = 6
    case codecLong = 7
    case chapterCount = 8
    case duration = 9
    case diskSize = 10
    case diskSizeBytes = 11
    case streamTypeExtension = 12
    case bitrate = 13
    case audioChannelsCount = 14
    case angleInfo = 15
    case sourceFileName = 16
    case audioSampleRate = 17
    case audioSampleSize = 18
    case videoSize = 19
    case videoAspectRatio = 20
    case videoFrameRate = 21
    case streamFlags = 22
    case dateTime = 23
    case originalTitleId = 24
    case segmentsCount = 25
    case segmentsMap = 26
    case outputFileName = 27
    case metadataLanguageCode = 28
    case metadataLanguageName = 29
    case treeInfo = 30
    case panelTitle = 31
    case volumeName = 32
    case orderWeight = 33
    case outputFormat = 34
    case outputFormatDescription = 35
    case seamlessInfo = 36
    case panelText = 37
    case mkvFlags = 38
    case mkvFlagsText = 39
    case audioChannelLayoutName = 40
    case outputCodecShort = 41
    case outputConversionType = 42
    case outputAudioSampleRate = 43
    case outputAudioSampleSize = 44
    case outputAudioChannelsCount = 45
    case outputAudioChannelLayoutName = 46
    case outputAudioChannelLayout = 47
    case outputAudioMixDescription = 48
    case comment = 49
    case offsetSequenceId = 50

    var displayName: String {
        switch self {
        case .unknown: return "Unknown"
        case .type: return "Type"
        case .name: return "Name"
        case .langCode: return "Language code"
        case .langName: return "Language"
        case .codecId: return "Codec ID"
        case .codecShort: return "Codec"
        case .codecLong: return "Codec (long)"
        case .chapterCount: return "Chapters"
        case .duration: return "Duration"
        case .diskSize: return "Size"
        case .diskSizeBytes: return "Size (bytes)"
        case .streamTypeExtension: return "Stream type extension"
        case .bitrate: return "Bitrate"
        case .audioChannelsCount: return "Channels"
        case .angleInfo: return "Angle"
        case .sourceFileName: return "Source file"
        case .audioSampleRate: return "Sample rate"
        case .audioSampleSize: return "Sample size"
        case .videoSize: return "Resolution"
        case .videoAspectRatio: return "Aspect ratio"
        case .videoFrameRate: return "Frame rate"
        case .streamFlags: return "Stream flags"
        case .dateTime: return "Date"
        case .originalTitleId: return "Source title ID"
        case .segmentsCount: return "Segment count"
        case .segmentsMap: return "Segment map"
        case .outputFileName: return "Output file name"
        case .metadataLanguageCode: return "Metadata language code"
        case .metadataLanguageName: return "Metadata language"
        case .treeInfo: return "Summary"
        case .panelTitle: return "Panel title"
        case .volumeName: return "Volume name"
        case .orderWeight: return "Order weight"
        case .outputFormat: return "Output format"
        case .outputFormatDescription: return "Output format description"
        case .seamlessInfo: return "Seamless info"
        case .panelText: return "Panel text"
        case .mkvFlags: return "MKV flags"
        case .mkvFlagsText: return "MKV flags (text)"
        case .audioChannelLayoutName: return "Channel layout"
        case .outputCodecShort: return "Output codec"
        case .outputConversionType: return "Conversion"
        case .outputAudioSampleRate: return "Output sample rate"
        case .outputAudioSampleSize: return "Output sample size"
        case .outputAudioChannelsCount: return "Output channels"
        case .outputAudioChannelLayoutName: return "Output channel layout"
        case .outputAudioChannelLayout: return "Output channel layout ID"
        case .outputAudioMixDescription: return "Output mix"
        case .comment: return "Comment"
        case .offsetSequenceId: return "Offset sequence ID"
        }
    }

    /// Attributes that are shown in the details panel. Internal/markup attributes are hidden.
    var isUserVisible: Bool {
        switch self {
        case .unknown, .panelTitle, .panelText, .treeInfo, .orderWeight, .streamTypeExtension, .outputAudioChannelLayout:
            return false
        default:
            return true
        }
    }
}

/// Drive state as reported in the second field of a DRV line.
enum DriveState: Int, Sendable, Codable {
    case emptyClosed = 0
    case emptyOpen = 1
    case inserted = 2
    case loading = 3
    case noDrive = 256
    case unmounting = 257

    init(raw: Int) { self = DriveState(rawValue: raw) ?? .noDrive }

    var displayName: String {
        switch self {
        case .emptyClosed: return "No disc"
        case .emptyOpen: return "Tray open"
        case .inserted: return "Disc inserted"
        case .loading: return "Loading…"
        case .noDrive: return "Not present"
        case .unmounting: return "Unmounting…"
        }
    }

    var hasDisc: Bool { self == .inserted }
}

/// Disc file system flags reported in the fourth field of a DRV line.
struct DiscFlags: OptionSet, Sendable, Codable, Hashable {
    let rawValue: Int
    static let dvdFiles = DiscFlags(rawValue: 1)
    static let hdDvdFiles = DiscFlags(rawValue: 2)
    static let blurayFiles = DiscFlags(rawValue: 4)
    static let aacsFiles = DiscFlags(rawValue: 8)
    static let bdsvmFiles = DiscFlags(rawValue: 16)

    var discTypeName: String {
        if contains(.blurayFiles) {
            return contains(.aacsFiles) ? "Blu-ray (AACS)" : "Blu-ray"
        }
        if contains(.hdDvdFiles) { return "HD DVD" }
        if contains(.dvdFiles) { return "DVD" }
        return "Disc"
    }

    /// Short token used by templates: "bd", "hddvd", "dvd" or "disc".
    var discTypeToken: String {
        if contains(.blurayFiles) { return "bd" }
        if contains(.hdDvdFiles) { return "hddvd" }
        if contains(.dvdFiles) { return "dvd" }
        return "disc"
    }
}

/// Stream flags (attribute 22).
struct StreamFlags: OptionSet, Sendable, Hashable {
    let rawValue: Int
    static let directorsComments = StreamFlags(rawValue: 1)
    static let alternateDirectorsComments = StreamFlags(rawValue: 2)
    static let forVisuallyImpaired = StreamFlags(rawValue: 4)
    static let coreAudio = StreamFlags(rawValue: 256)
    static let secondaryAudio = StreamFlags(rawValue: 512)
    static let hasCoreAudio = StreamFlags(rawValue: 1024)
    static let derivedStream = StreamFlags(rawValue: 2048)
    static let forcedSubtitles = StreamFlags(rawValue: 4096)
    static let profileSecondaryStream = StreamFlags(rawValue: 8192)
    static let offsetSequenceIdPresent = StreamFlags(rawValue: 16384)

    var descriptions: [String] {
        var out: [String] = []
        if contains(.directorsComments) { out.append("Director's comments") }
        if contains(.alternateDirectorsComments) { out.append("Alternate director's comments") }
        if contains(.forVisuallyImpaired) { out.append("For visually impaired") }
        if contains(.coreAudio) { out.append("Core audio") }
        if contains(.secondaryAudio) { out.append("Secondary audio") }
        if contains(.hasCoreAudio) { out.append("Has core audio") }
        if contains(.derivedStream) { out.append("Derived stream") }
        if contains(.forcedSubtitles) { out.append("Forced subtitles") }
        return out
    }
}

// MARK: - Events

struct RobotMessage: Equatable, Sendable, Codable {
    enum Severity: String, Sendable, Codable { case debug, info, warning, error }

    var code: Int
    var flags: Int
    var text: String
    var format: String
    var parameters: [String]

    /// Codes that always indicate a failure even when no error box flag is set.
    static let errorCodes: Set<Int> = [2003, 2004, 2023, 5003, 5010, 5021, 5037, 5055, 5069, 5077]
    static let warningCodes: Set<Int> = [3038, 3041, 5042]

    var severity: Severity {
        if code == 1003 || (flags & 0x20) != 0 && text.hasPrefix("DEBUG") { return .debug }
        if (flags & 0x200) != 0 || RobotMessage.errorCodes.contains(code) { return .error }
        if (flags & 0x400) != 0 || RobotMessage.warningCodes.contains(code) { return .warning }
        return .info
    }
}

struct DriveScanEntry: Equatable, Sendable, Codable, Hashable {
    var index: Int
    var state: DriveState
    var flags: DiscFlags
    var driveName: String
    var discName: String
    var devicePath: String

    var isPresent: Bool { state != .noDrive && !(driveName.isEmpty && devicePath.isEmpty) }
}

enum RobotEvent: Equatable, Sendable {
    case message(RobotMessage)
    /// PRGC: current (sub-)operation title.
    case progressCurrentTitle(code: Int, id: Int, name: String)
    /// PRGT: total operation title.
    case progressTotalTitle(code: Int, id: Int, name: String)
    /// PRGV: progress values. Fractions are `current / max` and `total / max`.
    case progressValue(current: Int, total: Int, max: Int)
    case drive(DriveScanEntry)
    case titleCount(Int)
    case discInfo(id: Int, code: Int, value: String)
    case titleInfo(title: Int, id: Int, code: Int, value: String)
    case streamInfo(title: Int, stream: Int, id: Int, code: Int, value: String)
    /// Any line that is not in robot format (e.g. usage errors printed to stderr).
    case raw(String)
}

// MARK: - Parser

enum RobotParser {
    /// Splits the comma separated field list of a robot line. Fields may be quoted; inside quotes
    /// `\"` and `\\` are unescaped and commas do not separate fields.
    static func splitFields(_ s: Substring) -> [String] {
        var fields: [String] = []
        var current = ""
        var inQuotes = false
        var iterator = s.makeIterator()
        var wasQuoted = false
        while let c = iterator.next() {
            if inQuotes {
                if c == "\\" {
                    if let next = iterator.next() {
                        if next == "\"" || next == "\\" {
                            current.append(next)
                        } else {
                            current.append(c)
                            current.append(next)
                        }
                    } else {
                        current.append(c)
                    }
                } else if c == "\"" {
                    inQuotes = false
                } else {
                    current.append(c)
                }
            } else {
                if c == "," {
                    fields.append(current)
                    current = ""
                    wasQuoted = false
                } else if c == "\"" && current.isEmpty && !wasQuoted {
                    inQuotes = true
                    wasQuoted = true
                } else {
                    current.append(c)
                }
            }
        }
        fields.append(current)
        return fields
    }

    static func parse(line rawLine: String) -> RobotEvent? {
        let line = rawLine.trimmingCharacters(in: CharacterSet(charactersIn: "\r\n"))
        if line.isEmpty { return nil }
        guard let colon = line.firstIndex(of: ":") else { return .raw(line) }
        let tag = line[..<colon]
        let body = line[line.index(after: colon)...]
        switch tag {
        case "MSG":
            let f = splitFields(body)
            guard f.count >= 5, let code = Int(f[0]), let flags = Int(f[1]) else { return .raw(line) }
            let params = Array(f.dropFirst(5))
            return .message(RobotMessage(code: code, flags: flags, text: f[3], format: f[4], parameters: params))
        case "PRGC", "PRGT":
            let f = splitFields(body)
            guard f.count >= 3, let code = Int(f[0]), let id = Int(f[1]) else { return .raw(line) }
            return tag == "PRGC"
                ? .progressCurrentTitle(code: code, id: id, name: f[2])
                : .progressTotalTitle(code: code, id: id, name: f[2])
        case "PRGV":
            let f = splitFields(body)
            guard f.count >= 3, let a = Int(f[0]), let b = Int(f[1]), let m = Int(f[2]) else { return .raw(line) }
            return .progressValue(current: a, total: b, max: m)
        case "DRV":
            let f = splitFields(body)
            guard f.count >= 7, let idx = Int(f[0]), let st = Int(f[1]), let flags = Int(f[3]) else { return .raw(line) }
            return .drive(DriveScanEntry(index: idx, state: DriveState(raw: st), flags: DiscFlags(rawValue: flags),
                                         driveName: f[4], discName: f[5], devicePath: f[6]))
        case "TCOUNT":
            guard let n = Int(body.trimmingCharacters(in: .whitespaces)) else { return .raw(line) }
            return .titleCount(n)
        case "CINFO":
            let f = splitFields(body)
            guard f.count >= 3, let id = Int(f[0]), let code = Int(f[1]) else { return .raw(line) }
            return .discInfo(id: id, code: code, value: f[2])
        case "TINFO":
            let f = splitFields(body)
            guard f.count >= 4, let t = Int(f[0]), let id = Int(f[1]), let code = Int(f[2]) else { return .raw(line) }
            return .titleInfo(title: t, id: id, code: code, value: f[3])
        case "SINFO":
            let f = splitFields(body)
            guard f.count >= 5, let t = Int(f[0]), let s = Int(f[1]), let id = Int(f[2]), let code = Int(f[3]) else { return .raw(line) }
            return .streamInfo(title: t, stream: s, id: id, code: code, value: f[4])
        default:
            return .raw(line)
        }
    }
}
