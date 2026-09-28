import Foundation

// Finds the episodes inside a DVD "play all" title by reading the disc's own navigation: the title
// tables in the IFO files and the jump commands of menus, buttons and title pre-commands. Works on an
// ISO image, a VIDEO_TS folder (backup) or a mounted disc. The Windows and Linux versions implement the
// same analysis.

/// Read access to the files of a VIDEO_TS directory.
protocol VideoTSReader: AnyObject {
    var label: String { get }
    /// File name (upper case, e.g. "VTS_01_0.IFO") → size in bytes.
    var files: [String: Int] { get }
    /// Reads `count` 2048-byte sectors starting at `sector` of `file`. Short reads return fewer bytes.
    func read(_ file: String, sector: Int, count: Int) -> Data
}

extension VideoTSReader {
    func readAll(_ file: String) -> Data {
        guard let size = files[file] else { return Data() }
        return read(file, sector: 0, count: (size + DVDNavigation.sector - 1) / DVDNavigation.sector).prefix(size)
    }
}

/// VIDEO_TS inside an ISO 9660 / UDF bridge image.
final class ISOVideoTS: VideoTSReader {
    let label: String
    private(set) var files: [String: Int] = [:]
    private var lba: [String: Int] = [:]
    private let handle: FileHandle

    init?(path: String) {
        guard let h = FileHandle(forReadingAtPath: path) else { return nil }
        handle = h
        let pvd = ISOVideoTS.readSectors(h, 16, 1)
        guard pvd.count >= 190, pvd[1..<6].elementsEqual("CD001".utf8) else { try? h.close(); return nil }
        label = String(decoding: pvd[40..<72], as: UTF8.self).trimmingCharacters(in: .whitespaces.union(.controlCharacters))
        let root = pvd.subdata(in: 156..<190)
        let entries = ISOVideoTS.directory(h, lba: Int(root.u32le(2)), size: Int(root.u32le(10)))
        guard let vts = entries.first(where: { $0.name.uppercased() == "VIDEO_TS" }) else { try? h.close(); return nil }
        for e in ISOVideoTS.directory(h, lba: vts.lba, size: vts.size) {
            files[e.name.uppercased()] = e.size
            lba[e.name.uppercased()] = e.lba
        }
    }

    deinit { try? handle.close() }

    func read(_ file: String, sector: Int, count: Int) -> Data {
        guard let start = lba[file] else { return Data() }
        return ISOVideoTS.readSectors(handle, start + sector, count)
    }

    private static func readSectors(_ h: FileHandle, _ lba: Int, _ count: Int) -> Data {
        do {
            try h.seek(toOffset: UInt64(lba) * UInt64(DVDNavigation.sector))
            return try h.read(upToCount: count * DVDNavigation.sector) ?? Data()
        } catch { return Data() }
    }

    private static func directory(_ h: FileHandle, lba: Int, size: Int) -> [(name: String, lba: Int, size: Int)] {
        let data = readSectors(h, lba, (size + DVDNavigation.sector - 1) / DVDNavigation.sector).prefix(size)
        var out: [(String, Int, Int)] = []
        var i = 0
        let bytes = [UInt8](data)
        while i < bytes.count {
            let len = Int(bytes[i])
            if len == 0 { i = (i / DVDNavigation.sector + 1) * DVDNavigation.sector; continue }
            guard i + len <= bytes.count, len >= 34 else { break }
            let rec = Data(bytes[i..<(i + len)])
            let nameLen = Int(rec[32])
            if 33 + nameLen <= rec.count {
                let raw = String(decoding: rec[33..<(33 + nameLen)], as: UTF8.self)
                let name = String(raw.split(separator: ";", omittingEmptySubsequences: false).first ?? "")
                if name != "\u{0}" && name != "\u{1}" {
                    out.append((name, Int(rec.u32le(2)), Int(rec.u32le(10))))
                }
            }
            i += len
        }
        return out
    }
}

/// A VIDEO_TS folder on disk (a backup, or a mounted disc).
final class FolderVideoTS: VideoTSReader {
    let label: String
    let files: [String: Int]
    private let dir: URL
    private var names: [String: String] = [:]

    /// `path` may be the VIDEO_TS folder itself or the folder that contains it.
    init?(path: String, label: String = "") {
        let fm = FileManager.default
        var d = URL(fileURLWithPath: path, isDirectory: true)
        if d.lastPathComponent.uppercased() != "VIDEO_TS" {
            guard let sub = (try? fm.contentsOfDirectory(atPath: d.path))?.first(where: { $0.uppercased() == "VIDEO_TS" }) else { return nil }
            d = d.appendingPathComponent(sub, isDirectory: true)
        }
        dir = d
        var f: [String: Int] = [:]
        for name in (try? fm.contentsOfDirectory(atPath: d.path)) ?? [] {
            let size = (try? fm.attributesOfItem(atPath: d.appendingPathComponent(name).path)[.size] as? Int) ?? 0
            f[name.uppercased()] = size
            names[name.uppercased()] = name
        }
        guard f["VIDEO_TS.IFO"] != nil else { return nil }
        files = f
        self.label = label.isEmpty ? d.deletingLastPathComponent().lastPathComponent : label
    }

    func read(_ file: String, sector: Int, count: Int) -> Data {
        guard let n = names[file], let h = FileHandle(forReadingAtPath: dir.appendingPathComponent(n).path) else { return Data() }
        defer { try? h.close() }
        do {
            try h.seek(toOffset: UInt64(sector) * UInt64(DVDNavigation.sector))
            return try h.read(upToCount: count * DVDNavigation.sector) ?? Data()
        } catch { return Data() }
    }
}

extension Data {
    func u8(_ o: Int) -> Int { o < count ? Int(self[startIndex + o]) : 0 }
    func u16be(_ o: Int) -> Int { (u8(o) << 8) | u8(o + 1) }
    func u32be(_ o: Int) -> Int { (u16be(o) << 16) | u16be(o + 2) }
    func u32le(_ o: Int) -> UInt32 { UInt32(u8(o)) | UInt32(u8(o + 1)) << 8 | UInt32(u8(o + 2)) << 16 | UInt32(u8(o + 3)) << 24 }
    func slice(_ from: Int) -> Data { from < count ? Data(self[(startIndex + from)...]) : Data() }
}

enum DVDNavigation {
    static let sector = 2048
    /// IFO times count 30 fps; NTSC playback is 29.97 fps.
    static let ntsc = 1.001

    struct Title: Equatable, Sendable {
        /// Chapter durations in seconds.
        var chapters: [Double]
        /// VOB id of each chapter's first cell.
        var chapterVOBs: [Int]
    }

    struct Analysis: Sendable {
        var label: String
        var titles: [Int: Title] = [:]
        /// Title number → chapter (PTT) → how it is reached, for every jump into a title found on the disc.
        var jumps: [Int: [Int: String]] = [:]
    }

    /// Where the episodes of one title start and end.
    struct EpisodePlan: Equatable, Sendable {
        var title: Int
        /// First chapter (1-based) of each episode.
        var starts: [Int]
        /// Last chapter of the last episode.
        var lastEnd: Int
        var endRule: String
        /// Chapters (≥ 1 s) played after the last episode, e.g. a closing clip.
        var tail: [Int]
        /// Start time of every chapter in seconds.
        var chapterStarts: [Double]
        /// Duration of the title without trailing sub-second chapters (what MakeMKV keeps).
        var duration: Double
        /// Number of chapters without those trailing sub-second chapters.
        var keptChapters: Int
        /// Episode durations in seconds.
        var episodeDurations: [Double]
        var reasons: [String]

        var episodeCount: Int { starts.count }
        /// Whether a ripped title with `chapters` chapters can be this title (MakeMKV may drop a trailing chapter).
        func matchesChapterCount(_ chapters: Int) -> Bool { (keptChapters - 1)...chapterStarts.count ~= chapters }

        /// Chapters at which to split (disc chapter numbers): every episode start after the first, then the tail.
        var splitChapters: [Int] { Array(starts.dropFirst()) + tail.prefix(1) }

        /// Chapter range of episode `i` (0-based).
        func chapterRange(_ i: Int) -> ClosedRange<Int> {
            let end = i + 1 < starts.count ? starts[i + 1] - 1 : lastEnd
            return starts[i]...max(starts[i], end)
        }

        /// Whether the episodes look like real episodes rather than, say, a movie's scene selection.
        /// `strict` is used when the plan alone decides that a disc is a TV show.
        func isPlausible(strict: Bool) -> Bool {
            guard episodeCount >= (strict ? 3 : 2), let lo = episodeDurations.min(), let hi = episodeDurations.max(), lo > 0 else { return false }
            return strict ? (lo >= 900 && hi / lo <= 1.35) : (lo >= 300 && hi / lo <= 2.0)
        }
    }

    // MARK: - IFO parsing

    static func bcd(_ b: Int) -> Int { (b >> 4) * 10 + (b & 0xF) }

    static func dvdTime(_ d: Data, _ o: Int) -> Double {
        let fps = (d.u8(o + 3) >> 6) == 1 ? 25.0 : 30.0
        return Double(bcd(d.u8(o)) * 3600 + bcd(d.u8(o + 1)) * 60 + bcd(d.u8(o + 2))) + Double(bcd(d.u8(o + 3) & 0x3F)) / fps
    }

    enum Jump: Equatable {
        /// Jump to a chapter; `titleNumber` is the VTS title number (nil = the current title).
        case ptt(titleNumber: Int?, ptt: Int, condition: String)
        /// Jump to a disc title.
        case title(Int, condition: String)
    }

    /// Decodes the DVD VM commands that jump into titles (LinkPTTN, JumpVTS_PTT, JumpTT).
    static func decodeJump(_ c: Data) -> Jump? {
        guard c.count >= 8, c.u8(0) >> 5 == 1 else { return nil }
        var cond = ""
        if c.u8(1) & 0x70 != 0 {
            let ops = [1: "&", 2: "==", 3: "!=", 4: ">=", 5: ">", 6: "<=", 7: "<"]
            let val = c.u8(1) & 0x80 != 0 ? String(c.u16be(4)) : "GPRM\(c.u8(5) & 0xF)"
            cond = "if GPRM\(c.u8(3) & 0xF) \(ops[(c.u8(1) >> 4) & 7] ?? "?") \(val): "
        }
        if c.u8(0) & 0x10 == 0 {
            if c.u8(1) & 0x0F == 5 { return .ptt(titleNumber: nil, ptt: c.u16be(6) & 0x3FF, condition: cond) }   // LinkPTTN
        } else {
            let sub = c.u8(1) & 0x0F
            if sub == 5 { return .ptt(titleNumber: c.u8(5) & 0x7F, ptt: c.u16be(2) & 0x3FF, condition: cond) } // JumpVTS_PTT
            if sub == 2 { return .title(c.u8(5) & 0x7F, condition: cond) }                                       // JumpTT
        }
        return nil
    }

    static func pgcCommands(_ pgc: Data) -> [Data] {
        let off = pgc.u16be(0xE4)
        guard off > 0 else { return [] }
        let n = pgc.u16be(off) + pgc.u16be(off + 2) + pgc.u16be(off + 4)
        return (0..<n).map { k in pgc.slice(off + 8 + 8 * k).prefix(8) }
    }

    /// Program chains of a PGCI table.
    static func pgcs(_ table: Data) -> [Data] {
        (0..<table.u16be(0)).map { i in table.slice(table.u32be(8 + 8 * i + 4)) }
    }

    /// Menu program chains of every language unit (`pointerOffset` = 0xC8 in VIDEO_TS.IFO, 0xD0 in VTS IFOs).
    static func menuPGCs(_ ifo: Data, pointerOffset: Int) -> [Data] {
        let sec = ifo.u32be(pointerOffset)
        guard sec > 0 else { return [] }
        let ut = ifo.slice(sec * sector)
        return (0..<ut.u16be(0)).flatMap { i in pgcs(ut.slice(ut.u32be(8 + 8 * i + 4))) }
    }

    static func isNav(_ s: Data) -> Bool {
        s.count >= 0x2D && s.u32be(0x0E) == 0x1BB && s.u32be(0x26) == 0x1BF && s.u8(0x2C) == 0
    }

    /// Commands of every highlight button in a menu VOB (read from the NAV packs).
    static func buttonCommands(_ r: VideoTSReader, _ vob: String) -> [Data] {
        guard let size = r.files[vob] else { return [] }
        var seen = Set<Data>()
        var out: [Data] = []
        let total = size / sector
        var s = 0
        while s < total {
            let chunk = r.read(vob, sector: s, count: min(512, total - s))
            if chunk.isEmpty { break }
            for k in 0..<(chunk.count / sector) {
                let sec = chunk.slice(k * sector).prefix(sector)
                guard isNav(sec) else { continue }
                let pci = sec.slice(0x2D)
                for b in 0..<min(pci.u8(0x60 + 16), 36) {
                    let cmd = Data(pci.slice(0x8E + 18 * b + 10).prefix(8))
                    if seen.insert(cmd).inserted { out.append(cmd) }
                }
            }
            s += 512
        }
        return out
    }

    static func analyse(_ r: VideoTSReader) -> Analysis? {
        let vmg = r.readAll("VIDEO_TS.IFO")
        guard vmg.count > 0xCC, vmg.prefix(12).elementsEqual("DVDVIDEO-VMG".utf8) else { return nil }
        var a = Analysis(label: r.label)
        let tt = vmg.slice(vmg.u32be(0xC4) * sector)
        var titles: [Int: (vts: Int, ttn: Int)] = [:]
        for i in 0..<tt.u16be(0) {
            let o = 8 + 12 * i
            titles[i + 1] = (tt.u8(o + 6), tt.u8(o + 7))
        }
        var byVtsTtn: [String: Int] = [:]
        for (t, v) in titles { byVtsTtn["\(v.vts):\(v.ttn)"] = t }
        func add(_ title: Int, _ ptt: Int, _ how: String) {
            if a.jumps[title]?[ptt] == nil { a.jumps[title, default: [:]][ptt] = how }
        }

        for p in menuPGCs(vmg, pointerOffset: 0xC8) {
            for c in pgcCommands(p) { if case let .title(t, _) = decodeJump(c) { add(t, 1, "menu JumpTT") } }
        }
        for c in buttonCommands(r, "VIDEO_TS.VOB") {
            if case let .title(t, _) = decodeJump(c) { add(t, 1, "button JumpTT") }
        }

        for vtsn in Set(titles.values.map(\.vts)).sorted() {
            let name = String(format: "VTS_%02d_0.IFO", vtsn)
            guard r.files[name] != nil else { continue }
            let ifo = r.readAll(name)
            let pb = ifo.slice(ifo.u32be(0xC8) * sector)
            let nttu = pb.u16be(0)
            var offs = (0..<nttu).map { pb.u32be(8 + 4 * $0) }
            offs.append(pb.u32be(4) + 1)
            let titlePGCs = pgcs(ifo.slice(ifo.u32be(0xCC) * sector))
            guard nttu > 0 else { continue }
            for ttn in 1...nttu {
                guard let title = byVtsTtn["\(vtsn):\(ttn)"] else { continue }
                var chapters: [Double] = [], vobs: [Int] = []
                var firstPGC: Int?
                var j = offs[ttn - 1]
                while j + 4 <= offs[ttn] {
                    let pgcn = pb.u16be(j), pgn = pb.u16be(j + 2)
                    j += 4
                    guard pgcn >= 1, pgcn <= titlePGCs.count else { continue }
                    if firstPGC == nil { firstPGC = pgcn }
                    let pg = titlePGCs[pgcn - 1]
                    let nprog = pg.u8(2), ncell = pg.u8(3)
                    let pmo = pg.u16be(0xE6), cpbo = pg.u16be(0xE8), cpso = pg.u16be(0xEA)
                    var pmap = (0..<nprog).map { pg.u8(pmo + $0) }
                    pmap.append(ncell + 1)
                    guard pgn >= 1, pgn < pmap.count else { continue }
                    var d = 0.0
                    for cell in max(0, pmap[pgn - 1] - 1)..<max(0, pmap[pgn] - 1) { d += dvdTime(pg, cpbo + 24 * cell + 4) }
                    chapters.append(ntsc * d)
                    vobs.append(pg.u16be(cpso + 4 * (pmap[pgn - 1] - 1)))
                }
                a.titles[title] = Title(chapters: chapters, chapterVOBs: vobs)
                if let fp = firstPGC {
                    for c in pgcCommands(titlePGCs[fp - 1]) {
                        if case let .ptt(nil, ptt, cond) = decodeJump(c) { add(title, ptt, "\(cond)LinkPTTN \(ptt)") }
                    }
                }
            }
            var cmds = menuPGCs(ifo, pointerOffset: 0xD0).flatMap(pgcCommands)
            cmds += buttonCommands(r, String(format: "VTS_%02d_0.VOB", vtsn))
            for c in cmds {
                if case let .ptt(ttn?, ptt, _) = decodeJump(c), let t = byVtsTtn["\(vtsn):\(ttn)"] {
                    add(t, ptt, "menu JumpVTS_PTT \(ttn):\(ptt)")
                }
            }
        }
        return a
    }

    // MARK: - Episode plan

    /// Episode layout of `title`, when the disc's menus jump into two or more of its chapters.
    static func plan(_ a: Analysis, title: Int) -> EpisodePlan? {
        guard let t = a.titles[title], !t.chapters.isEmpty, var targets = a.jumps[title] else { return nil }
        if targets[1] == nil { targets[1] = "title start" }
        let starts = targets.keys.filter { $0 >= 1 && $0 <= t.chapters.count }.sorted()
        guard starts.count >= 2 else { return nil }
        let ch = t.chapters, vobs = t.chapterVOBs
        // Where does the last episode end? Discs like these store each episode as its own VOB, so an
        // episode runs while the chapters stay in the VOB of its first chapter. If the title is a single
        // VOB, assume the last episode has as many chapters as the one before it.
        var lastEnd = starts.last!
        let endRule: String
        if Set(vobs).count > 1 {
            while lastEnd < ch.count && vobs[lastEnd] == vobs[starts.last! - 1] { lastEnd += 1 }
            endRule = "VOB boundary"
        } else {
            lastEnd = min(ch.count, starts.last! + (starts.last! - starts[starts.count - 2]) - 1)
            endRule = "same chapter count as the previous episode"
        }
        let tail = ((lastEnd + 1)...max(lastEnd + 1, ch.count)).filter { $0 <= ch.count && ch[$0 - 1] >= 1.0 }
        let real = (1...ch.count).filter { !($0 > lastEnd && ch[$0 - 1] < 1.0) }
        let chapterStarts = (0..<ch.count).map { ch.prefix($0).reduce(0, +) }
        var plan = EpisodePlan(title: title, starts: starts, lastEnd: lastEnd, endRule: endRule, tail: tail,
                               chapterStarts: chapterStarts, duration: real.reduce(0) { $0 + ch[$1 - 1] }, keptChapters: real.count,
                               episodeDurations: [], reasons: starts.map { targets[$0] ?? "" })
        plan.episodeDurations = (0..<starts.count).map { i in plan.chapterRange(i).reduce(0) { $0 + ch[$1 - 1] } }
        return plan
    }

    /// Plans for every title that the menus jump into at two or more chapters, most episodes first.
    static func plans(_ a: Analysis) -> [EpisodePlan] {
        a.jumps.keys.compactMap { plan(a, title: $0) }.sorted { ($0.episodeCount, -$0.title) > ($1.episodeCount, -$1.title) }
    }

    // MARK: - Episode numbers from the menus

    /// Raw MPEG-PS data of every menu still (one per VOB/cell id) — input for OCR.
    static func menuStills(_ r: VideoTSReader) -> [Data] {
        var out: [Data] = []
        for name in r.files.keys.sorted() where name == "VIDEO_TS.VOB" || name.range(of: #"^VTS_\d\d_0\.VOB$"#, options: .regularExpression) != nil {
            let size = r.files[name] ?? 0
            guard size >= 64 * 1024, size <= 512 * 1024 * 1024 else { continue }
            let data = r.readAll(name)
            let total = data.count / sector
            var cells: [String: (Int, Int)] = [:]
            for s in 0..<total {
                let sec = data.slice(s * sector).prefix(sector)
                guard isNav(sec) else { continue }
                let dsi = sec.slice(0x407)
                let key = "\(dsi.u16be(0x18)):\(dsi.u8(0x1B))"
                if let v = cells[key] { cells[key] = (v.0, s) } else { cells[key] = (s, s) }
            }
            let bounds = cells.values.sorted { $0.0 < $1.0 }
            for (i, b) in bounds.enumerated() {
                let end = i + 1 < bounds.count ? bounds[i + 1].0 : total
                if end - b.0 > 8 { out.append(data.slice(b.0 * sector).prefix((end - b.0) * sector)) }
            }
        }
        return out
    }

    /// Episode numbers in OCR text ("EPISODE 138", "Episode #12").
    static func episodeNumbers(inText text: String) -> Set<Int> {
        var out = Set<Int>()
        let re = try! NSRegularExpression(pattern: #"EPIS[O0]DE\s*#?\s*(\d{1,4})\b"#, options: [.caseInsensitive])
        for m in re.matches(in: text, range: NSRange(text.startIndex..., in: text)) {
            if let r = Range(m.range(at: 1), in: text), let n = Int(text[r]) { out.insert(n) }
        }
        return out
    }

    /// The first episode number S such that [S, S+count) covers the most numbers read from the menus;
    /// nil when there is no unique best choice backed by at least two numbers.
    static func firstEpisode(from numbers: Set<Int>, count: Int) -> Int? {
        guard !numbers.isEmpty, count > 0 else { return nil }
        var scores: [Int: Int] = [:]
        for n in numbers { for k in 0..<count where n - k >= 0 { scores[n - k] = 0 } }
        for s in scores.keys { scores[s] = numbers.filter { s <= $0 && $0 < s + count }.count }
        guard let best = scores.values.max() else { return nil }
        let winners = scores.filter { $0.value == best }.map(\.key)
        return winners.count == 1 && best >= 2 ? winners[0] : nil
    }

    /// Maps disc chapter numbers to chapter numbers of the MKV, using the MKV's chapter start times when known.
    static func mkvChapters(for discChapters: [Int], plan: EpisodePlan, mkvStarts: [Double]?) -> [Int]? {
        guard let st = mkvStarts, !st.isEmpty else { return discChapters }
        var out: [Int] = []
        for c in discChapters {
            let want = plan.chapterStarts[c - 1]
            guard let best = st.indices.min(by: { abs(st[$0] - want) < abs(st[$1] - want) }), abs(st[best] - want) <= 1.0 else { return nil }
            out.append(best + 1)
        }
        return out
    }

    static func hms(_ s: Double) -> String {
        String(format: "%d:%02d:%06.3f", Int(s) / 3600, Int(s) % 3600 / 60, s.truncatingRemainder(dividingBy: 60))
    }
}
