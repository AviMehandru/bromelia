import Foundation

/// A single track (stream) of a title.
struct TrackInfo: Identifiable, Hashable, Sendable, Codable {
    enum Kind: String, Sendable, Codable { case video, audio, subtitle, attachment, other }

    var index: Int
    var attributes: [Int: String] = [:]

    var id: Int { index }

    func attribute(_ a: AttributeID) -> String? { attributes[a.rawValue] }

    var kind: Kind {
        switch attributes[AttributeID.type.rawValue]?.lowercased() {
        case "video"?: return .video
        case "audio"?: return .audio
        case "subtitles"?, "subtitle"?: return .subtitle
        case "attachment"?: return .attachment
        default: return .other
        }
    }

    var languageCode: String { attribute(.langCode) ?? "" }
    var languageName: String { attribute(.langName) ?? "" }
    var codec: String { attribute(.codecShort) ?? attribute(.codecId) ?? "" }
    var flags: StreamFlags { StreamFlags(rawValue: Int(attribute(.streamFlags) ?? "0") ?? 0) }
    var isDefault: Bool { (attribute(.mkvFlags) ?? "").contains("d") }
    var isForced: Bool { flags.contains(.forcedSubtitles) || (attribute(.mkvFlags) ?? "").contains("f") }

    /// Human readable one-line summary, similar to MakeMKV's tree view.
    var summary: String {
        if let tree = attribute(.treeInfo), !tree.trimmingCharacters(in: .whitespaces).isEmpty {
            return tree.trimmingCharacters(in: .whitespaces)
        }
        return [attribute(.codecShort), attribute(.name), attribute(.langName)]
            .compactMap { $0 }.filter { !$0.isEmpty }.joined(separator: " ")
    }
}

/// A title as listed by makemkvcon (index = position in the TINFO list, used by the `mkv` command).
struct TitleInfo: Identifiable, Hashable, Sendable, Codable {
    var index: Int
    var attributes: [Int: String] = [:]
    var tracks: [TrackInfo] = []

    var id: Int { index }

    func attribute(_ a: AttributeID) -> String? { attributes[a.rawValue] }

    var name: String { attribute(.name) ?? "" }
    var chapterCount: Int { Int(attribute(.chapterCount) ?? "") ?? 0 }
    var durationText: String { attribute(.duration) ?? "" }
    var durationSeconds: Int { TitleInfo.parseDuration(durationText) }
    var sizeBytes: Int64 { Int64(attribute(.diskSizeBytes) ?? "") ?? 0 }
    var sizeText: String { attribute(.diskSize) ?? ByteCountFormatter.string(fromByteCount: sizeBytes, countStyle: .file) }
    var sourceTitleId: Int? { Int(attribute(.originalTitleId) ?? "") }
    var segmentMap: String { attribute(.segmentsMap) ?? "" }
    var outputFileName: String { attribute(.outputFileName) ?? "" }
    var sourceFileName: String { attribute(.sourceFileName) ?? "" }
    var comment: String { attribute(.comment) ?? "" }
    var angle: Int? { attribute(.angleInfo).flatMap { Int($0) } }

    var displayName: String {
        if let tree = attribute(.treeInfo), !tree.isEmpty { return tree }
        return "\(chapterCount) chapter(s), \(sizeText)"
    }

    static func parseDuration(_ s: String) -> Int {
        let parts = s.split(separator: ":").map { Int($0) ?? 0 }
        return parts.reduce(0) { $0 * 60 + $1 }
    }

    static func formatDuration(_ seconds: Int) -> String {
        String(format: "%d:%02d:%02d", seconds / 3600, (seconds / 60) % 60, seconds % 60)
    }
}

/// Everything makemkvcon reported about an opened disc (or ISO / folder).
struct DiscInfo: Hashable, Sendable, Codable {
    var attributes: [Int: String] = [:]
    var titles: [TitleInfo] = []
    var reportedTitleCount: Int = 0

    func attribute(_ a: AttributeID) -> String? { attributes[a.rawValue] }

    var name: String { attribute(.name) ?? attribute(.volumeName) ?? "" }
    var volumeName: String { attribute(.volumeName) ?? "" }
    var typeName: String { attribute(.type) ?? "" }

    var typeToken: String {
        let t = typeName.lowercased()
        if t.contains("blu") { return "bd" }
        if t.contains("hd") { return "hddvd" }
        if t.contains("dvd") { return "dvd" }
        return "disc"
    }

    func title(at index: Int) -> TitleInfo? { titles.first { $0.index == index } }
}

/// Incrementally assembles a `DiscInfo` from robot events.
struct DiscInfoBuilder {
    private(set) var info = DiscInfo()

    mutating func consume(_ event: RobotEvent) {
        switch event {
        case .titleCount(let n):
            info.reportedTitleCount = n
        case let .discInfo(id, _, value):
            info.attributes[id] = value
        case let .titleInfo(t, id, _, value):
            ensureTitle(t)
            if let i = info.titles.firstIndex(where: { $0.index == t }) {
                info.titles[i].attributes[id] = value
            }
        case let .streamInfo(t, s, id, _, value):
            ensureTitle(t)
            guard let ti = info.titles.firstIndex(where: { $0.index == t }) else { return }
            if !info.titles[ti].tracks.contains(where: { $0.index == s }) {
                info.titles[ti].tracks.append(TrackInfo(index: s))
                info.titles[ti].tracks.sort { $0.index < $1.index }
            }
            if let si = info.titles[ti].tracks.firstIndex(where: { $0.index == s }) {
                info.titles[ti].tracks[si].attributes[id] = value
            }
        default:
            break
        }
    }

    private mutating func ensureTitle(_ t: Int) {
        if !info.titles.contains(where: { $0.index == t }) {
            info.titles.append(TitleInfo(index: t))
            info.titles.sort { $0.index < $1.index }
        }
    }

    var hasData: Bool { !info.titles.isEmpty || !info.attributes.isEmpty }

    static func build(fromOutput text: String) -> DiscInfo {
        var b = DiscInfoBuilder()
        text.enumerateLines { line, _ in
            if let ev = RobotParser.parse(line: line) { b.consume(ev) }
        }
        return b.info
    }
}
