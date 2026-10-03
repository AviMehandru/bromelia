/// Finds the episodes inside a DVD "play all" title by reading the disc's own navigation: the title tables in the IFO
/// files and the jump commands of menus, buttons and title pre-commands (was DVDNavigation).
public enum DvdNav {
    static let sector = 2048
    /// IFO times count 30 fps; NTSC playback is 29.97 fps.
    static let ntsc = 1.001

    // MARK: Bounds-checked reads (0 past the end)

    static func u8(_ d: [UInt8], _ o: Int) -> Int { o >= 0 && o < d.count ? Int(d[o]) : 0 }
    static func u16(_ d: [UInt8], _ o: Int) -> Int { (u8(d, o) << 8) | u8(d, o + 1) }
    static func u32(_ d: [UInt8], _ o: Int) -> Int { (u16(d, o) << 16) | u16(d, o + 2) }
    static func slice(_ d: [UInt8], _ from: Int) -> [UInt8] { from >= 0 && from < d.count ? Array(d[from...]) : [] }
    static func slice(_ d: [UInt8], _ from: Int, _ len: Int) -> [UInt8] {
        from >= 0 && from < d.count ? Array(d[from..<min(d.count, from + len)]) : []
    }

    static func bcd(_ b: Int) -> Int { (b >> 4) * 10 + (b & 0xF) }

    static func dvdTime(_ d: [UInt8], _ o: Int) -> Double {
        let fps = (u8(d, o + 3) >> 6) == 1 ? 25.0 : 30.0
        return Double(bcd(u8(d, o)) * 3600 + bcd(u8(d, o + 1)) * 60 + bcd(u8(d, o + 2))) + Double(bcd(u8(d, o + 3) & 0x3F)) / fps
    }

    /// An 8-byte VM command: LinkPTTN, JumpVTS_PTT or JumpTT with its condition; nil for any other.
    public static func decodeJump(_ bytes: [UInt8]) -> Jump? {
        let c = bytes
        guard c.count >= 8, c[0] >> 5 == 1 else { return nil }
        var cond = ""
        if c[1] & 0x70 != 0 {
            let ops = ["?", "&", "==", "!=", ">=", ">", "<=", "<"]
            let val = c[1] & 0x80 != 0 ? String(u16(c, 4)) : "GPRM\(c[5] & 0xF)"
            cond = "if GPRM\(c[3] & 0xF) \(ops[Int((c[1] >> 4) & 7)]) \(val): "
        }
        if c[0] & 0x10 == 0 {
            if c[1] & 0x0F == 5 { return .ptt(titleNumber: nil, chapter: u16(c, 6) & 0x3FF, condition: cond) }  // LinkPTTN
        } else {
            let sub = c[1] & 0x0F
            if sub == 5 { return .ptt(titleNumber: Int(c[5] & 0x7F), chapter: u16(c, 2) & 0x3FF, condition: cond) }  // JumpVTS_PTT
            if sub == 2 { return .title(number: Int(c[5] & 0x7F), condition: cond) }  // JumpTT
        }
        return nil
    }

    static func pgcCommands(_ pgc: [UInt8]) -> [[UInt8]] {
        let off = u16(pgc, 0xE4)
        guard off != 0 else { return [] }
        let n = u16(pgc, off) + u16(pgc, off + 2) + u16(pgc, off + 4)
        return (0..<n).map { slice(pgc, off + 8 + 8 * $0, 8) }
    }

    static func pgcs(_ table: [UInt8]) -> [[UInt8]] { (0..<u16(table, 0)).map { slice(table, u32(table, 8 + 8 * $0 + 4)) } }

    static func menuPgcs(_ ifo: [UInt8], _ pointerOffset: Int) -> [[UInt8]] {
        let sec = u32(ifo, pointerOffset)
        guard sec != 0 else { return [] }
        let ut = slice(ifo, sec * sector)
        return (0..<u16(ut, 0)).flatMap { pgcs(slice(ut, u32(ut, 8 + 8 * $0 + 4))) }
    }

    static func isNav(_ d: [UInt8], _ o: Int) -> Bool {
        o + 0x2D <= d.count && u32(d, o + 0x0E) == 0x1BB && u32(d, o + 0x26) == 0x1BF && d[o + 0x2C] == 0
    }

    /// One pass over a menu VOB: the button commands of its NAV packs (each once, in order) and a still per VOB/cell
    /// id longer than 8 sectors (in menu VOBs of 64 KiB to 512 MiB).
    static func scanMenuVob(_ source: ByteSource, _ name: String, _ size: Int64) -> (buttons: [[UInt8]], stills: [CellRef]) {
        var buttons: [[UInt8]] = []
        var seen = Set<[UInt8]>()
        var cells: [[Int]: (first: Int64, last: Int64)] = [:]
        let total = size / Int64(sector)
        var s: Int64 = 0
        while s < total {
            let chunk = source.read(name, offset: s * Int64(sector), length: Int(min(512, total - s)) * sector)
            if chunk.isEmpty { break }
            for k in 0..<(chunk.count / sector) {
                let o = k * sector
                guard isNav(chunk, o) else { continue }
                let pci = o + 0x2D
                for b in 0..<min(u8(chunk, pci + 0x60 + 16), 36) {
                    let cmd = slice(chunk, pci + 0x8E + 18 * b + 10, 8)
                    if seen.insert(cmd).inserted { buttons.append(cmd) }
                }
                let dsi = o + 0x407
                let key = [u16(chunk, dsi + 0x18), u8(chunk, dsi + 0x1B)]
                let at = s + Int64(k)
                cells[key] = cells[key].map { ($0.first, at) } ?? (at, at)
            }
            s += 512
        }
        var stills: [CellRef] = []
        if size >= 64 * 1024 && size <= 512 * 1024 * 1024 {
            let bounds = cells.values.sorted { $0.first < $1.first }
            for (i, b) in bounds.enumerated() {
                let end = i + 1 < bounds.count ? bounds[i + 1].first : total
                if end - b.first > 8 { stills.append(CellRef(file: name, firstSector: b.first, endSector: end)) }
            }
        }
        return (buttons, stills)
    }

    static func isMenuVob(_ name: String) -> Bool {
        if name == "VIDEO_TS.VOB" { return true }
        let u = Array(name.utf8)
        return u.count == 12 && name.hasPrefix("VTS_") && name.hasSuffix("_0.VOB") && (48...57).contains(u[4]) && (48...57).contains(u[5])
    }

    static func twoDigits(_ n: Int) -> String { n < 10 ? "0\(n)" : "\(n)" }

    /// The titles, chapter jumps and menu stills of the VIDEO_TS in `source`; nil when VIDEO_TS.IFO isn't a video
    /// manager.
    public static func analyse(_ source: ByteSource) -> NavAnalysis? {
        var files: [String: Int64] = [:]
        for f in source.files() { files[f.name] = f.size }
        func readAll(_ name: String) -> [UInt8] { files[name].map { source.read(name, offset: 0, length: Int($0)) } ?? [] }
        let vmg = readAll("VIDEO_TS.IFO")
        guard vmg.count > 0xCC, String(decoding: vmg[0..<12], as: UTF8.self) == "DVDVIDEO-VMG" else { return nil }
        var menus: [String: (buttons: [[UInt8]], stills: [CellRef])] = [:]
        let menuNames = files.keys.filter(isMenuVob).sorted { $0.utf8.lexicographicallyPrecedes($1.utf8) }
        for name in menuNames { menus[name] = scanMenuVob(source, name, files[name]!) }

        var titleList: [NavTitle] = []
        var jumps: [NavJump] = []
        var seenJumps = Set<[Int]>()
        func add(_ title: Int, _ ptt: Int, _ how: String) {
            if seenJumps.insert([title, ptt]).inserted { jumps.append(NavJump(title: title, chapter: ptt, how: how)) }
        }

        let tt = slice(vmg, u32(vmg, 0xC4) * sector)
        var titles: [Int: [Int]] = [:]
        for i in 0..<u16(tt, 0) { titles[i + 1] = [u8(tt, 8 + 12 * i + 6), u8(tt, 8 + 12 * i + 7)] }
        var byVtsTtn: [[Int]: Int] = [:]
        for (k, v) in titles.sorted(by: { $0.key < $1.key }) { byVtsTtn[v] = k }

        for p in menuPgcs(vmg, 0xC8) {
            for c in pgcCommands(p) { if case .title(let n, _) = decodeJump(c) { add(n, 1, "menu JumpTT") } }
        }
        for c in menus["VIDEO_TS.VOB"]?.buttons ?? [] { if case .title(let n, _) = decodeJump(c) { add(n, 1, "button JumpTT") } }

        for vtsn in Set(titles.values.map { $0[0] }).sorted() {
            let name = "VTS_\(twoDigits(vtsn))_0.IFO"
            guard files[name] != nil else { continue }
            let ifo = readAll(name)
            let pb = slice(ifo, u32(ifo, 0xC8) * sector)
            let nttu = u16(pb, 0)
            var offs = (0..<nttu).map { u32(pb, 8 + 4 * $0) }
            offs.append(u32(pb, 4) + 1)
            let titlePgcs = pgcs(slice(ifo, u32(ifo, 0xCC) * sector))
            if nttu > 0 {
                for ttn in 1...nttu {
                    guard let title = byVtsTtn[[vtsn, ttn]] else { continue }
                    var chapters: [Double] = []
                    var vobs: [Int] = []
                    var firstPgc: Int?
                    var j = offs[ttn - 1]
                    while j + 4 <= offs[ttn] {
                        defer { j += 4 }
                        let pgcn = u16(pb, j), pgn = u16(pb, j + 2)
                        guard pgcn >= 1, pgcn <= titlePgcs.count else { continue }
                        if firstPgc == nil { firstPgc = pgcn }
                        let pg = titlePgcs[pgcn - 1]
                        let nprog = u8(pg, 2), ncell = u8(pg, 3)
                        let pmo = u16(pg, 0xE6), cpbo = u16(pg, 0xE8), cpso = u16(pg, 0xEA)
                        var pmap = (0..<nprog).map { u8(pg, pmo + $0) }
                        pmap.append(ncell + 1)
                        guard pgn >= 1, pgn < pmap.count else { continue }
                        var d = 0.0
                        var cell = max(0, pmap[pgn - 1] - 1)
                        while cell < max(0, pmap[pgn] - 1) {
                            d += dvdTime(pg, cpbo + 24 * cell + 4)
                            cell += 1
                        }
                        chapters.append(ntsc * d)
                        vobs.append(u16(pg, cpso + 4 * (pmap[pgn - 1] - 1)))
                    }
                    titleList.removeAll { $0.number == title }
                    titleList.append(NavTitle(number: title, chapters: chapters, chapterVobs: vobs))
                    if let fp = firstPgc {
                        for c in pgcCommands(titlePgcs[fp - 1]) {
                            if case .ptt(nil, let chapter, let condition) = decodeJump(c) { add(title, chapter, "\(condition)LinkPTTN \(chapter)") }
                        }
                    }
                }
            }
            let cmds = menuPgcs(ifo, 0xD0).flatMap(pgcCommands) + (menus["VTS_\(twoDigits(vtsn))_0.VOB"]?.buttons ?? [])
            for c in cmds {
                if case .ptt(let ttn2?, let chapter, _) = decodeJump(c), let t = byVtsTtn[[vtsn, ttn2]] {
                    add(t, chapter, "menu JumpVTS_PTT \(ttn2):\(chapter)")
                }
            }
        }
        return NavAnalysis(titles: titleList, jumps: jumps, stills: menuNames.flatMap { menus[$0]!.stills })
    }

    static func range(_ starts: [Int], _ lastEnd: Int, _ i: Int) -> ChapterRange {
        let end = i + 1 < starts.count ? starts[i + 1] - 1 : lastEnd
        return ChapterRange(first: starts[i], last: max(starts[i], end))
    }

    /// The episodes of title `titleIndex`: an episode starts at every chapter a menu, button or pre-command jumps to,
    /// and at chapter 1. The last one ends at the next VOB boundary (or, in a single VOB, after as many chapters as
    /// the one before it). Nil without at least two starts.
    public static func episodePlan(_ analysis: NavAnalysis, titleIndex: Int) -> EpisodePlan? {
        let jumps = analysis.jumps.filter { $0.title == titleIndex }
        guard let t = analysis.titles.first(where: { $0.number == titleIndex }), !t.chapters.isEmpty, !jumps.isEmpty else { return nil }
        var targets: [Int: String] = [:]
        for j in jumps where targets[j.chapter] == nil { targets[j.chapter] = j.how }
        if targets[1] == nil { targets[1] = "title start" }
        let ch = t.chapters, vobs = t.chapterVobs
        let starts = targets.keys.filter { $0 >= 1 && $0 <= ch.count }.sorted()
        guard starts.count >= 2 else { return nil }
        let last = starts[starts.count - 1]
        var lastEnd = last
        let endRule: String
        if Set(vobs).count > 1 {
            while lastEnd < ch.count && vobs[lastEnd] == vobs[last - 1] { lastEnd += 1 }
            endRule = "VOB boundary"
        } else {
            lastEnd = min(ch.count, last + (last - starts[starts.count - 2]) - 1)
            endRule = "same chapter count as the previous episode"
        }
        let tail = lastEnd < ch.count ? (lastEnd + 1...ch.count).filter { ch[$0 - 1] >= 1.0 } : []
        let real = (1...ch.count).filter { !($0 > lastEnd && ch[$0 - 1] < 1.0) }
        var chapterStarts: [Double] = []
        var acc = 0.0
        for c in ch {
            chapterStarts.append(acc)
            acc += c
        }
        let durations = (0..<starts.count).map { i -> Double in
            let r = range(starts, lastEnd, i)
            return (r.first...r.last).reduce(0.0) { $0 + ch[$1 - 1] }
        }
        return EpisodePlan(title: titleIndex, starts: starts, lastEnd: lastEnd, endRule: endRule, tail: tail,
                           splitChapters: Array(starts.dropFirst()) + tail.prefix(1), duration: real.reduce(0.0) { $0 + ch[$1 - 1] },
                           chapterStarts: chapterStarts, keptChapters: real.count, episodeDurations: durations, reasons: starts.map { targets[$0]! })
    }

    /// Plans for every title that the menus jump into at two or more chapters, most episodes first.
    public static func plans(_ analysis: NavAnalysis) -> [EpisodePlan] {
        var titles: [Int] = []
        for j in analysis.jumps where !titles.contains(j.title) { titles.append(j.title) }
        return titles.compactMap { episodePlan(analysis, titleIndex: $0) }
            .sorted { $0.starts.count != $1.starts.count ? $0.starts.count > $1.starts.count : $0.title < $1.title }
    }

    /// The menu stills (one per VOB/cell id): input for MenuOcr.
    public static func stillCells(_ analysis: NavAnalysis) -> [CellRef] { analysis.stills }

    /// The disc chapters of episode `episode` (0-based).
    public static func chapterRange(_ plan: EpisodePlan, episode: Int) -> ChapterRange { range(plan.starts, plan.lastEnd, episode) }

    /// Whether the episodes look like real episodes rather than a movie's scene selection: strict, at least 3 of 15
    /// minutes or more within 35 % of each other; otherwise at least 2 of 5 minutes or more within a factor of 2.
    public static func isPlausible(_ plan: EpisodePlan, strict: Bool) -> Bool {
        guard plan.starts.count >= (strict ? 3 : 2), let lo = plan.episodeDurations.min(), let hi = plan.episodeDurations.max(), lo > 0 else { return false }
        return strict ? lo >= 900 && hi / lo <= 1.35 : lo >= 300 && hi / lo <= 2.0
    }

    /// Whether a ripped title with `chapters` chapters is this plan's title (MakeMKV may drop one short chapter).
    public static func matchesChapterCount(_ plan: EpisodePlan, chapters: Int) -> Bool {
        chapters >= plan.keptChapters - 1 && chapters <= plan.chapterStarts.count
    }

    /// The MKV chapters to split at for the disc chapters `split`, by the MKV's chapter starts (which may begin with
    /// an added chapter 00); the disc's numbers when the starts are unknown; nil when a chapter has no MKV chapter
    /// within 1 s.
    public static func mkvChapters(_ split: [Int], plan: EpisodePlan, mkvStarts: [Double]?) -> [Int]? {
        guard let mkvStarts, !mkvStarts.isEmpty else { return split }
        var result: [Int] = []
        for c in split {
            let want = plan.chapterStarts[c - 1]
            var best = 0
            for i in 1..<mkvStarts.count where abs(mkvStarts[i] - want) < abs(mkvStarts[best] - want) { best = i }
            if abs(mkvStarts[best] - want) > 1.0 { return nil }
            result.append(best + 1)
        }
        return result
    }
}
