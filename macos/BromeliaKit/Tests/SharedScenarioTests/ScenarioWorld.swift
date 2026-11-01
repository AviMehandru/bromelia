import BroDomain
import BroFoundation
import BroPorts
import BroTestSupport
import Foundation

/// The world a scenario starts in (decision 235), on the test root: `{root}` is `TestCompositionRoot.root` and paths use
/// '/' everywhere. The configuration (given.config, else config3-scenario-base.json, then configPatch, then profile over
/// the default profile, `{root}` filled in) is written to the data folder before the engine starts; each library gets its
/// folder and marker; given.files are written; given.disc is what the ISO (and a drive given no disc) holds, and `iso`
/// exists as a small file. The drive world (decision 237): each fake drive's device, identification, MakeMKV index, tray
/// and disc, which the drive scan's DRV lines report (no disc while the tray is open); the engine's eject opens the tray,
/// leaving the disc in it (the OS reports TrayOpened), and its close shuts it unless the drive has no tray motor. The tools
/// are the scripts (given.tools; makemkvcon's default is tools/makemkvcon-saves.yaml).
final class ScenarioWorld: @unchecked Sendable {
    static let iso = TestCompositionRoot.root + "/test.iso"
    private static let defaultConfig = "config3-scenario-base.json"
    private static let defaultMakemkvcon = "tools/makemkvcon-saves.yaml"

    /// A fake drive: its scenario name, device, identification, MakeMKV index, tray and disc.
    final class Drive: @unchecked Sendable {
        var name = ""
        var device = ""
        var identification = ""
        var index = 0
        var trayOpen = false
        var trayMotor = true
        var disc: JsonValue?
    }

    private let lock = NSLock()
    let given: JsonValue
    let drives: [Drive]
    private var scripts: [String: JsonValue] = [:]
    /// The marker written in each library's folder, by library id.
    private var markers: [String: LibraryMarker] = [:]
    private var currentDisc: JsonValue?

    init(_ scenario: JsonValue) throws {
        given = scenario["given"] ?? .object([])
        currentDisc = try given["disc"].map(ScenarioCatalog.disc)
        scripts["makemkvcon"] = try ScenarioCatalog.read(Self.defaultMakemkvcon)
        for (tool, script) in given["tools"]?.members ?? [] {
            if script.isNull { scripts.removeValue(forKey: tool) } else { scripts[tool] = try ScenarioCatalog.script(script) }
        }
        var made: [Drive] = []
        for (order, d) in (given["drives"]?.array ?? []).enumerated() {
            let drive = Drive()
            let id = d["id"]?.string ?? ""
            drive.name = id
            drive.device = d["device"]?.string ?? "/dev/" + id
            drive.identification = d["identification"]?.string ?? d["name"]?.string ?? "BD-RE " + id
            drive.index = Int(d["makemkvIndex"]?.int ?? Int64(order))
            drive.trayMotor = d["trayMotor"]?.bool ?? true
            drive.disc = try d["disc"].map(ScenarioCatalog.disc)
            made.append(drive)
        }
        drives = made
    }

    /// Whether the tool has a script (the locator finds exactly those).
    func installed(_ tool: String) -> Bool { lock.withLock { scripts[tool] != nil } }

    /// The tool's script from now on (null: not installed).
    func setScript(_ tool: String, _ script: JsonValue?) throws {
        let loaded = try script.flatMap { $0.isNull ? nil : try ScenarioCatalog.script($0) }
        lock.withLock { scripts[tool] = loaded }
    }

    func script(_ tool: String) -> JsonValue? { lock.withLock { scripts[tool] } }

    /// What the ISO (and a folder or file source) holds.
    var disc: JsonValue? {
        get { lock.withLock { currentDisc } }
        set { lock.withLock { currentDisc = newValue } }
    }

    func driveNamed(_ name: String) throws -> Drive {
        guard let d = drives.first(where: { $0.name == name }) else { throw ScenarioFailure("a drive " + name, "none") }
        return d
    }

    // MARK: The root

    /// Writes the configuration, libraries, files and the ISO; adds the drives and hooks the tray calls.
    func prepare(_ root: TestCompositionRoot) async throws {
        let fs = root.fs
        let config = try configuration()
        fs.put(TestCompositionRoot.configPath, JsonValue.encodeCanonical(config))
        var marked: [String: Bool] = [:]
        for l in given["libraries"]?.array ?? [] { marked[l["id"]?.string ?? ""] = l["marker"]?.bool ?? true }
        for library in config["libraries"]?.array ?? [] {
            let id = library["id"]?.string ?? "", path = library["path"]?.string ?? ""
            fs.putFolder(path)
            if marked[id] ?? true {
                let marker = LibraryMarker.make(id, path: path, name: library["name"]?.string ?? id, created: root.clock.now())
                fs.put(path + "/" + LibraryMarker.fileName, LibraryMarker.encode(marker))
                lock.withLock { markers[id] = marker }
            }
        }
        for (path, content) in given["files"]?.members ?? [] { fs.put(TestCompositionRoot.root + "/" + path, Self.fileBytes(content)) }
        if let free = given["freeBytes"]?.int { fs.anyVolume?.freeBytes = free }
        for (n, unit) in (given["units"]?.array ?? []).enumerated() { try await addUnit(root, config, unit, n + 1) }
        if let disc { fs.put(Self.iso, Array(("an image of " + (disc["volume"]?.string ?? "a disc")).utf8)) }
        for d in drives {
            root.drives.add(OsDrive(device: d.device, identification: d.identification))
            root.drives.setContent(d.device, Self.content(d.disc))
        }
        let drives = root.drives
        root.drives.onEject = { [self] device in
            change(device) { $0.trayOpen = true } // the disc stays in the open tray until a remove
            drives.send(.trayOpened(device: device)) // and the OS reports it, as for openTray
        }
        root.drives.onCloseTray = { [self] device in change(device) { $0.trayOpen = !$0.trayMotor } }
    }

    /// An archive unit from before (given.units, README "Catalogue"): its folder with its files ("contents of <name>, long
    /// enough to hash"), SHA256SUMS and a version 3 record bromelia-<unit8>.json, and unless inCatalogue is false the rows a
    /// commit makes (UnitRepository.finishCommit: the committed unit and its files). Its id is
    /// 00000000-0000-4000-8000-2000000000<n>; its library is given.library, else the one whose folder holds it; its
    /// fingerprint is given, or fingerprintOf's (the Domain fingerprint of that disc's generated listing).
    private func addUnit(_ root: TestCompositionRoot, _ config: JsonValue, _ unit: JsonValue, _ n: Int) async throws {
        if unit["record"]?.int == 2 { throw ScenarioFailure("given.units[].record 2 (phase 5)", "not built") }
        let fs = root.fs
        var folderBytes = Array(((unit["folder"]?.string) ?? "").utf8)
        while folderBytes.last == UInt8(ascii: "/") { folderBytes.removeLast() }
        let folder = TestCompositionRoot.root + "/" + String(decoding: folderBytes, as: UTF8.self)
        let libraries = config["libraries"]?.array ?? []
        let library: JsonValue? = if let wanted = unit["library"]?.string {
            libraries.first { $0["id"]?.string == wanted }
        } else {
            libraries.first { folder.utf8.starts(with: (Self.trimSlash($0["path"]?.string ?? "") + "/").utf8) }
        }
        guard let library else { throw ScenarioFailure("a library holding " + folder, "none") }
        let libraryPath = Self.trimSlash(library["path"]?.string ?? "")
        let libraryId = library["id"]?.string ?? ""
        let id = Id("00000000-0000-4000-8000-2000000000" + String(format: "%02x", n))
        let unit8 = String(id.value.prefix(8))
        let fingerprint = try unit["fingerprint"]?.string ?? unit["fingerprintOf"].flatMap { Fingerprint.of(try Self.parsedListing(ScenarioCatalog.disc($0))) }
        let status = unit["status"]?.string ?? "success"
        let kind = unit["kind"]?.string ?? "movie"
        let name = unit["name"]?.string ?? Self.lastPart(folder)
        let at = unit["archivedAt"]?.string.flatMap { Instant.parse($0 + "T20:00:00.000Z") } ?? root.clock.now()
        func int(_ key: String) -> Int? { unit[key]?.int.map { Int($0) } }
        func number(_ key: String) -> JsonValue { unit[key]?.int.map { .integer($0) } ?? .null }

        fs.putFolder(folder)
        var files: [UnitFile] = []
        for file in (unit["files"]?.array ?? []).compactMap(\.string) {
            let bytes = Array("contents of \(file), long enough to hash".utf8)
            fs.put(folder + "/" + file, bytes)
            files.append(UnitFile(path: file, size: Int64(bytes.count), sha256: Sha256Sums.hash(bytes), role: file.lowercased().hasSuffix(".mkv") ? "title" : ""))
        }
        fs.put(folder + "/" + Sha256Sums.fileName, Array(Sha256Sums.render(files.map { SumEntry(path: $0.path, sha256: $0.sha256) }).utf8))
        let recordFile = "bromelia-\(unit8).json"
        let document = JsonValue.object([
            ("format", .string("bromelia-archive")), ("version", .integer(3)),
            ("unit", .object([("id", .string(id.value)), ("short", .string(unit8)), ("library", .string(libraryId))])),
            ("status", .string(status)), ("name", .string(name)), ("kind", .string(kind)),
            ("disc", .object([("label", .string(unit["label"]?.string ?? "")), ("fingerprint", fingerprint.map { .string($0) } ?? .null),
                              ("season", number("season")), ("part", number("part")), ("volume", number("volume")), ("disc", number("disc"))])),
            ("episodes", unit["episodes"] ?? .array([])),
            ("files", .array(files.map { .object([("path", .string($0.path)), ("size", .integer($0.size)), ("sha256", .string($0.sha256))]) })),
            ("archivedAt", .string(Instant.format(at))),
        ])
        guard let decoded = ArchiveRecordCodec.decode(JsonValue.encodeCanonical(document)) else {
            throw ScenarioFailure("a version 3 record for " + folder, "none")
        }
        fs.put(folder + "/" + recordFile, ArchiveRecordCodec.encodeV3(decoded))
        if unit["inCatalogue"]?.bool == false { return }
        let relative = String(decoding: folder.utf8.dropFirst(libraryPath.utf8.count + 1), as: UTF8.self)
        let record = UnitRecord(id: id, libraryId: libraryId, physicalDiscId: nil, jobId: nil, path: relative, recordFile: recordFile, recordVersion: 3,
                                state: .committed, status: UnitStatus(rawValue: status) ?? .success, name: name, kind: MediaKind(rawValue: kind) ?? .movie,
                                format: "bluray", formatCode: "BR", encrypted: false, fingerprint: fingerprint, label: unit["label"]?.string ?? "",
                                season: int("season"), part: int("part"), volume: int("volume"), disc: int("disc"), makemkvVersion: "",
                                bytes: files.reduce(0) { $0 + $1.size }, fileCount: files.count, attempts: 1, createdAt: at, committedAt: at)
        let intent = CommitIntent(unitId: id, jobId: id, staging: "", destination: folder, merge: false, quarantine: false, createdAt: at, items: [])
        // The library's row as attaching it left it (the unit's row needs it), with the marker the folder holds.
        let markerId = lock.withLock { markers[libraryId]?.id } ?? "none"
        let row = LibraryRecord(id: libraryId, name: library["name"]?.string ?? libraryId, path: libraryPath, markerId: markerId, attachedAt: at,
                                online: true, lastSeenAt: at)
        let store = root.store
        try await store.libraries().upsert(row)
        try await store.units().finishCommit(intent, record: record, files: files)
    }

    /// The configuration document the engine starts with.
    func configuration() throws -> JsonValue {
        var config = try Fixtures.json("config/" + (given["config"]?.string ?? Self.defaultConfig))
        if let patch = given["configPatch"] { config = Self.mergePatch(config, patch) }
        if let libraries = given["libraries"]?.array {
            config = Self.mergePatch(config, .object([("libraries", .array(libraries.map { l in
                .object([("id", l["id"] ?? .null), ("name", l["name"] ?? l["id"] ?? .null), ("path", .string(Self.absolute(l["path"]?.string ?? "")))])
            }))]))
        }
        if let profile = given["profile"] {
            var profiles = config["profiles"]?.array ?? []
            if let at = profiles.firstIndex(where: { $0["id"]?.string == "default" }) {
                profiles[at] = Self.mergePatch(profiles[at], profile)
            } else {
                profiles.append(Self.mergePatch(.object([("id", .string("default"))]), profile))
            }
            config = Self.mergePatch(config, .object([("profiles", .array(profiles))]))
        }
        return Self.fill(config)
    }

    /// A path the scenario gives relative to the root, as an absolute one ({root} stays for `fill`).
    private static func absolute(_ path: String) -> String { path.hasPrefix("/") || path.hasPrefix("{root}") ? path : "{root}/" + path }

    private static func trimSlash(_ path: String) -> String {
        var bytes = Array(path.utf8)
        while bytes.last == UInt8(ascii: "/") { bytes.removeLast() }
        return String(decoding: bytes, as: UTF8.self)
    }

    private static func lastPart(_ path: String) -> String {
        guard let i = path.utf8.lastIndex(of: UInt8(ascii: "/")) else { return path }
        return String(decoding: path.utf8[path.utf8.index(after: i)...], as: UTF8.self)
    }

    /// RFC 7386.
    static func mergePatch(_ target: JsonValue, _ patch: JsonValue) -> JsonValue {
        guard let p = patch.members else { return patch }
        var members = target.members ?? []
        for (key, value) in p {
            let at = members.firstIndex { $0.key == key }
            if value.isNull {
                if let at { members.remove(at: at) }
                continue
            }
            let merged = mergePatch(at.map { members[$0].value } ?? .null, value)
            if let at { members[at] = (key, merged) } else { members.append((key, merged)) }
        }
        return .object(members)
    }

    /// {root} in every string.
    static func fill(_ v: JsonValue) -> JsonValue {
        switch v {
        case .string(let s): return .string(s.replacingOccurrences(of: "{root}", with: TestCompositionRoot.root))
        case .array(let a): return .array(a.map(fill))
        case .object(let m): return .object(m.map { ($0.key, fill($0.value)) })
        default: return v
        }
    }

    /// A file's bytes from a scenario: text, or {bytes, seed, descriptor} (that many bytes of a seeded pattern, a volume
    /// descriptor at 32769).
    static func fileBytes(_ content: JsonValue) -> [UInt8] {
        if let text = content.string { return Array(text.utf8) }
        let count = Int(content["bytes"]?.int ?? 0), seed = Int(content["seed"]?.int ?? 0)
        var bytes = (0..<count).map { UInt8(($0 * 31 + seed * 7) & 0xFF) }
        if let descriptor = content["descriptor"]?.string, bytes.count >= 32769 + descriptor.utf8.count {
            bytes.replaceSubrange(32769..<32769 + descriptor.utf8.count, with: Array(descriptor.utf8))
        }
        return bytes
    }

    private static func content(_ disc: JsonValue?) -> DiscContent {
        guard let disc else { return .unknown }
        return DiscContent(rawValue: disc["content"]?.string ?? "video") ?? .video
    }

    // MARK: The drive world

    private func change(_ device: String, _ change: (Drive) -> Void) {
        lock.withLock { for d in drives where d.device == device { change(d) } }
    }

    /// A disc goes into the drive (its tray closes) and the OS reports it.
    func insert(_ root: TestCompositionRoot, _ name: String, _ disc: JsonValue) throws {
        let d = try driveNamed(name)
        let loaded = try ScenarioCatalog.disc(disc)
        lock.withLock {
            d.disc = loaded
            d.trayOpen = false
        }
        root.drives.setContent(d.device, Self.content(loaded))
        root.drives.send(.mediaArrived(device: d.device))
    }

    func remove(_ root: TestCompositionRoot, _ name: String) throws {
        let d = try driveNamed(name)
        lock.withLock { d.disc = nil }
        root.drives.setContent(d.device, .unknown)
        root.drives.send(.mediaRemoved(device: d.device))
    }

    func openTray(_ root: TestCompositionRoot, _ name: String) throws {
        let d = try driveNamed(name)
        lock.withLock { d.trayOpen = true }
        root.drives.send(.trayOpened(device: d.device))
    }

    /// A drive's disc (nil while its tray is open).
    func discIn(_ d: Drive) -> JsonValue? { lock.withLock { d.disc } }

    /// The DRV lines of a drive scan: index, state (2 a disc, 1 an open tray, 0 empty), the disc's flags (DVD 1, HD DVD
    /// 2, Blu-ray 4 + AACS 8), identification, label and device.
    func driveScan() -> [String] {
        lock.withLock {
            drives.sorted { $0.index < $1.index }.map { d in
                let disc = d.trayOpen ? nil : d.disc
                let state = disc != nil ? 2 : d.trayOpen ? 1 : 0
                var flags: Int
                switch disc?["type"]?.string {
                case nil: flags = 0
                case "dvd": flags = 1
                case "hddvd": flags = 2
                default: flags = 12
                }
                if let given = disc?["flags"]?.int { flags = Int(given) }
                let label = disc?["label"]?.string ?? disc?["volume"]?.string ?? ""
                return "DRV:\(d.index),\(state),999,\(flags),\"\(d.identification)\",\"\(label)\",\"\(d.device)\""
            }
        }
    }

    /// The disc a makemkvcon source names: disc:N or dev:<device> a drive's, anything else the ISO's.
    func discOf(_ source: String) -> JsonValue? {
        lock.withLock {
            if source.hasPrefix("disc:"), let index = Int(source.dropFirst(5)) {
                guard let d = drives.first(where: { $0.index == index }), !d.trayOpen else { return nil }
                return d.disc
            }
            if source.hasPrefix("dev:") {
                let device = String(source.dropFirst(4))
                guard let d = drives.first(where: { $0.device == device }), !d.trayOpen else { return nil }
                return d.disc
            }
            return currentDisc
        }
    }

    /// The device a makemkvcon source names ("" for an image).
    func deviceOf(_ source: String) -> String {
        lock.withLock {
            if source.hasPrefix("disc:"), let index = Int(source.dropFirst(5)) { return drives.first { $0.index == index }?.device ?? "" }
            return source.hasPrefix("dev:") ? String(source.dropFirst(4)) : ""
        }
    }

    // MARK: Listings

    /// A disc's listing (README "A generated listing", or its listing fixture), without the titles shorter than
    /// `minLength` seconds, which renumbers the rest.
    static func listing(_ disc: JsonValue, minLength: Int? = nil) throws -> String {
        if let fixture = disc["listing"]?.string { return try Fixtures.text(fixture) }
        let titles = (disc["titles"]?.array ?? []).filter { t in
            guard let m = minLength else { return true }
            return seconds(t["duration"]?.string ?? "0") >= m
        }
        return Fixtures.generatedListing(.object([("volume", disc["volume"] ?? .string("")), ("type", disc["type"] ?? .string("bluray")),
                                                  ("titles", .array(titles))]))
    }

    /// The listing as the engine reads it (Robot.parseLine, ListingBuilder).
    static func parsedListing(_ disc: JsonValue) throws -> Listing {
        var builder = ListingBuilder()
        for line in lines(try listing(disc)) { if let e = Robot.parseLine(line) { builder.feed(e) } }
        return builder.build()
    }

    /// The non-empty lines of a text, split at the byte '\n' (Swift's Character "\r\n" is one grapheme), each without a
    /// final '\r' when `trimCR`.
    static func lines(_ text: String, trimCR: Bool = false) -> [String] {
        text.utf8.split(separator: UInt8(ascii: "\n")).map { line in
            var bytes = Array(line)
            if trimCR, bytes.last == UInt8(ascii: "\r") { bytes.removeLast() }
            return String(decoding: bytes, as: UTF8.self)
        }.filter { !$0.isEmpty }
    }

    /// h:mm:ss as seconds.
    static func seconds(_ duration: String) -> Int { duration.split(separator: ":").reduce(0) { $0 * 60 + (Int($1) ?? 0) } }
}
