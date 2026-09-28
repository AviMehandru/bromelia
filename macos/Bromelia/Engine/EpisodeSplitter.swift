import Foundation

/// Runs the external tools used to split DVD "play all" titles into episodes: `mkvextract` (chapter
/// times), `ffmpeg` + `tesseract` (episode numbers from menu screens) and `mkvmerge --split`.
enum EpisodeSplitter {
    /// Finds a command-line tool on PATH, in Homebrew's folders or in an MKVToolNix app bundle.
    /// GUI apps don't inherit the shell's PATH, so the usual install locations are searched explicitly.
    static func findTool(_ name: String, near sibling: URL? = nil) -> URL? {
        let fm = FileManager.default
        var dirs: [String] = []
        if let sibling { dirs.append(sibling.deletingLastPathComponent().path) }
        dirs += (ProcessInfo.processInfo.environment["PATH"] ?? "").split(separator: ":").map(String.init)
        dirs += ["/opt/homebrew/bin", "/usr/local/bin", "/usr/bin", "/opt/local/bin"]
        if let apps = try? fm.contentsOfDirectory(atPath: "/Applications") {
            dirs += apps.filter { $0.hasPrefix("MKVToolNix") }.sorted().reversed().map { "/Applications/\($0)/Contents/MacOS" }
        }
        for d in dirs where !d.isEmpty {
            let p = (d as NSString).appendingPathComponent(name)
            if fm.isExecutableFile(atPath: p) { return URL(fileURLWithPath: p) }
        }
        return nil
    }

    /// Mount point of a disc in a drive (/dev/rdisk4 → /Volumes/LABEL), for reading VIDEO_TS directly.
    static func mountPoint(device: String) async -> String? {
        let name = (device as NSString).lastPathComponent.replacingOccurrences(of: "rdisk", with: "disk")
        guard !name.isEmpty else { return nil }
        let collector = LineCollector()
        let runner = ProcessRunner(executable: URL(fileURLWithPath: "/usr/sbin/diskutil"), arguments: ["info", "-plist", name])
        guard let out = try? await runner.run(timeout: 30, onLine: { collector.append($0) }), out.exitCode == 0,
              let data = collector.joined.data(using: .utf8),
              let plist = try? PropertyListSerialization.propertyList(from: data, format: nil) as? [String: Any],
              let mp = plist["MountPoint"] as? String, !mp.isEmpty else { return nil }
        return mp
    }

    /// Chapter start times of an MKV (seconds), read with `mkvextract chapters --simple`.
    static func chapterStarts(of file: URL, mkvextract: URL?) async -> [Double]? {
        guard let mkvextract else { return nil }
        let tmp = FileManager.default.temporaryDirectory.appendingPathComponent("bromelia-chapters-\(UUID().uuidString).txt")
        defer { try? FileManager.default.removeItem(at: tmp) }
        let runner = ProcessRunner(executable: mkvextract, arguments: [file.path, "chapters", "--simple", tmp.path])
        guard let out = try? await runner.run(timeout: 120, onLine: { _ in }), out.exitCode <= 1,
              let text = try? String(contentsOf: tmp, encoding: .utf8) else { return nil }
        return parseSimpleChapters(text)
    }

    /// Parses `CHAPTER01=00:23:36.815` lines.
    static func parseSimpleChapters(_ text: String) -> [Double] {
        var out: [Double] = []
        for line in text.split(whereSeparator: \.isNewline) {
            guard let m = line.firstMatch(of: /^CHAPTER\d+=(\d+):(\d+):([\d.]+)/) else { continue }
            out.append(Double(Int(m.1)! * 3600 + Int(m.2)! * 60) + (Double(m.3) ?? 0))
        }
        return out
    }

    /// Duration of an MKV in seconds (`mkvmerge -J`).
    static func duration(of file: URL, mkvmerge: URL) async -> Double? {
        let collector = LineCollector()
        let runner = ProcessRunner(executable: mkvmerge, arguments: ["-J", file.path])
        guard let out = try? await runner.run(timeout: 120, onLine: { collector.append($0) }), out.exitCode == 0,
              let data = collector.joined.data(using: .utf8),
              let obj = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let props = (obj["container"] as? [String: Any])?["properties"] as? [String: Any],
              let ns = props["duration"] as? NSNumber else { return nil }
        return ns.doubleValue / 1e9
    }

    /// Reads episode numbers from the disc's menu screens with ffmpeg + tesseract.
    /// Returns nil (with a reason) when the tools are missing.
    static func ocrEpisodeNumbers(_ reader: VideoTSReader, isCancelled: @escaping @Sendable () -> Bool) async -> (Set<Int>?, String?) {
        guard let ffmpeg = findTool("ffmpeg"), let tesseract = findTool("tesseract") else {
            return (nil, "ffmpeg and tesseract are needed to read episode numbers from the menus")
        }
        let dir = FileManager.default.temporaryDirectory.appendingPathComponent("bromelia-ocr-\(UUID().uuidString)", isDirectory: true)
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: dir) }
        var found = Set<Int>()
        for (i, still) in DVDNavigation.menuStills(reader).enumerated() {
            if isCancelled() { break }
            let mpg = dir.appendingPathComponent("menu\(i).mpg"), png = dir.appendingPathComponent("menu\(i).png")
            guard (try? still.write(to: mpg)) != nil else { continue }
            let ff = ProcessRunner(executable: ffmpeg, arguments: ["-v", "quiet", "-y", "-f", "mpeg", "-i", mpg.path, "-frames:v", "1",
                                                                   "-vf", "scale=2160:1440,format=gray", png.path])
            guard let r = try? await ff.run(timeout: 60, onLine: { _ in }), r.exitCode == 0,
                  FileManager.default.fileExists(atPath: png.path) else { continue }
            let text = LineCollector()
            let tess = ProcessRunner(executable: tesseract, arguments: [png.path, "stdout", "--psm", "11"])
            _ = try? await tess.run(timeout: 60, onLine: { text.append($0) })
            found.formUnion(DVDNavigation.episodeNumbers(inText: text.joined))
        }
        return (found, nil)
    }

    /// Splits `file` at the given MKV chapters without re-encoding. Returns the parts in order
    /// (hidden temporary names in the same folder), or nil when mkvmerge failed.
    static func split(_ file: URL, atChapters chapters: [Int], mkvmerge: URL, register: (ProcessRunner?) -> Void,
                      log: @escaping @Sendable (String) -> Void) async -> [URL]? {
        let dir = file.deletingLastPathComponent()
        let prefix = ".bromelia-split-\(UUID().uuidString.prefix(8))"
        let args = ["-o", dir.appendingPathComponent("\(prefix)-%03d.mkv").path,
                    "--split", "chapters:" + chapters.map(String.init).joined(separator: ","), file.path]
        let runner = ProcessRunner(executable: mkvmerge, arguments: args)
        log("$ " + runner.commandLine)
        register(runner)
        defer { register(nil) }
        let out = try? await runner.run(onLine: { _ in })
        let parts = ((try? FileManager.default.contentsOfDirectory(at: dir, includingPropertiesForKeys: nil)) ?? [])
            .filter { $0.lastPathComponent.hasPrefix(prefix) }.sorted { $0.lastPathComponent < $1.lastPathComponent }
        // mkvmerge exits with 1 for warnings; the output is still valid.
        guard let out, out.exitCode <= 1, parts.count == chapters.count + 1 else {
            for p in parts { try? FileManager.default.removeItem(at: p) }
            return nil
        }
        return parts
    }
}
