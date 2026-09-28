import Foundation

enum JobError: LocalizedError {
    case message(String)
    var errorDescription: String? {
        switch self { case .message(let m): return m }
    }
}

/// Receives output lines on a background queue, parses them and delivers events on the main
/// queue in order. Progress values are coalesced to at most ten updates per second.
final class EventSink: @unchecked Sendable {
    private let lock = NSLock()
    private var pendingProgress: RobotEvent?
    private var progressScheduled = false
    private var closed = false
    private let deliver: @MainActor (RobotEvent) -> Void

    init(deliver: @escaping @MainActor (RobotEvent) -> Void) { self.deliver = deliver }

    func receive(_ line: String) {
        guard let ev = RobotParser.parse(line: line) else { return }
        if case .progressValue = ev {
            lock.lock()
            pendingProgress = ev
            let schedule = !progressScheduled
            progressScheduled = true
            lock.unlock()
            if schedule {
                DispatchQueue.main.asyncAfter(deadline: .now() + 0.1) { [self] in
                    MainActor.assumeIsolated { self.flushProgress() }
                }
            }
            return
        }
        DispatchQueue.main.async { [self] in
            MainActor.assumeIsolated {
                if !self.isClosed { self.deliver(ev) }
            }
        }
    }

    private var isClosed: Bool { lock.lock(); defer { lock.unlock() }; return closed }

    @MainActor private func flushProgress() {
        lock.lock()
        let ev = pendingProgress
        pendingProgress = nil
        progressScheduled = false
        let isClosed = closed
        lock.unlock()
        if let ev, !isClosed { deliver(ev) }
    }

    /// Waits until every event queued so far has been delivered, then stops delivering.
    @MainActor func drain() async {
        await withCheckedContinuation { (c: CheckedContinuation<Void, Never>) in
            DispatchQueue.main.async { c.resume() }
        }
        flushProgress()
        lock.lock(); closed = true; lock.unlock()
    }
}

/// Executes a single `RipJob`.
@MainActor
final class JobRunner {
    struct RunSummary {
        var exitCode: Int32 = 0
        var saved: Int?
        var failed: Int?
        var errorMessages: [String] = []
        var info = DiscInfoBuilder()
        var driveMismatch: String?
    }

    let job: RipJob
    let config: AppConfig
    let makemkvcon: URL
    let mkvmerge: URL?
    var onFinished: ((RipJob) -> Void)?

    private var env: MakeMKVEnvironment!
    private var baseEnv: MakeMKVEnvironment!
    private var allTracksEnv: MakeMKVEnvironment?
    private var activeRunner: ProcessRunner?
    private(set) var cancelRequested = false
    private let cancelFlag = CancelFlag()
    private var summary = RunSummary()
    private var expectedDrive: (index: Int, device: String)?
    private var lastTotalTitle = ""
    // Episodes (see prepareEpisodes)
    private var videoTS: VideoTSReader?
    private var episodePlan: DVDNavigation.EpisodePlan?
    private var separateEpisodes: [Int: Int] = [:]   // MakeMKV title → episode offset
    private var firstEpisodeNumber = 1
    private var episodeWidth = 2
    private var fileTitles: [URL: Int] = [:]

    init(job: RipJob, config: AppConfig, makemkvcon: URL, mkvmerge: URL?) {
        self.job = job
        self.config = config
        self.makemkvcon = makemkvcon
        self.mkvmerge = mkvmerge
    }

    func cancel() {
        cancelRequested = true
        cancelFlag.set()
        job.phase = "Cancelling…"
        activeRunner?.cancel()
    }

    // MARK: - Lifecycle

    func run() async {
        job.state = .running
        job.startedAt = Date()
        job.phase = "Starting"
        try? Paths.ensureDirectory(job.logDirectory)
        FileManager.default.createFile(atPath: job.logFile.path, contents: nil)
        job.logHandle = try? FileHandle(forWritingTo: job.logFile)
        job.appendLog("Bromelia job \(job.id.uuidString)")
        job.appendLog("\(job.mode.label) from \(job.source.displayName) using configuration “\(job.drive.name)”")

        var status: JobState = .succeeded
        do {
            try await execute()
        } catch {
            if cancelRequested || error is CancellationError {
                status = .cancelled
                job.errorMessage = "Cancelled by user"
                job.appendLog("Job cancelled", severity: .warning)
            } else {
                status = .failed
                job.errorMessage = error.localizedDescription
                job.appendLog(error.localizedDescription, severity: .error)
            }
        }
        if cancelRequested && status == .succeeded { status = .cancelled }

        // Post-processing.
        writeManifest(status: status)
        let steps = applicableSteps(status: status)
        if !steps.isEmpty && !cancelRequested {
            job.phase = "Post-processing"
            job.currentOperation = ""
            let context = PostProcessor.Context(status: status, values: templateValues(status: status),
                                                outputDirectory: job.outputDirectory, files: job.producedFiles,
                                                manifestPath: job.manifestFile.path, environment: scriptEnvironment(status: status))
            let results = await PostProcessor.run(steps: steps, context: context, register: { [weak self] r in
                self?.activeRunner = r
            }, log: { [weak self] text, sev in
                DispatchQueue.main.async { MainActor.assumeIsolated { self?.job.appendLog(text, severity: sev) } }
            })
            await flushMainQueue()
            for r in results where r.exitCode != 0 || r.timedOut {
                if let step = steps.first(where: { $0.id == r.stepId }), step.failJobOnError, status == .succeeded {
                    status = .failed
                    job.errorMessage = "Post-processing step “\(r.name)” failed"
                }
            }
            if cancelRequested && status == .succeeded { status = .cancelled }
        }

        // Eject.
        if case let .drive(_, device) = job.source,
           (status == .succeeded && job.drive.automation.ejectWhenDone) || (status == .failed && job.drive.automation.ejectOnFailure) {
            job.phase = "Ejecting"
            let ok = await DiscEjector.eject(devicePath: device)
            job.appendLog(ok ? "Disc ejected" : "Could not eject \(device)", severity: ok ? .info : .warning)
        }

        job.state = status
        job.finishedAt = Date()
        job.phase = status.label
        job.currentOperation = ""
        if status == .succeeded { job.totalProgress = 1; job.currentProgress = 1 }
        writeManifest(status: status)
        job.appendLog("Finished: \(status.label) in \(Formatters.duration(job.elapsed))")
        try? job.logHandle?.close()
        job.logHandle = nil
        if job.drive.archive.archiveRecord, let dir = job.outputDirectory, !job.producedFiles.isEmpty {
            try? FileManager.default.copyItem(at: job.logFile, to: Paths.uniqueURL(dir.appendingPathComponent("bromelia-log.txt")))
        }

        if job.drive.automation.notify {
            let body: String
            switch status {
            case .succeeded: body = "\(job.producedFiles.count) item(s) saved to \(job.outputDirectory?.path ?? "")"
            case .cancelled: body = "Cancelled"
            default: body = job.errorMessage ?? "Failed"
            }
            Notifier.post(title: "\(job.mode.shortLabel) \(status == .succeeded ? "finished" : status.label.lowercased()): \(job.discLabel.isEmpty ? job.sourceLabel : job.discLabel)",
                          body: body, sound: job.drive.automation.playSound)
        }
        onFinished?(job)
    }

    private func flushMainQueue() async {
        await withCheckedContinuation { (c: CheckedContinuation<Void, Never>) in DispatchQueue.main.async { c.resume() } }
    }

    // MARK: - Pipeline

    private func execute() async throws {
        let needsAllTracks = job.mode.makesMKV && !job.trackSelections.isEmpty
        if needsAllTracks && mkvmerge == nil {
            throw JobError.message("Choosing individual tracks requires mkvmerge (MKVToolNix). Install MKVToolNix or reset the track choices.")
        }
        let home = job.logDirectory.appendingPathComponent("home", isDirectory: true)
        env = try MakeMKVEnvironment.prepare(executable: makemkvcon, config: config, drive: job.drive, home: home)
        baseEnv = env
        if needsAllTracks {
            // Titles with hand-picked tracks are ripped with every track, then trimmed with mkvmerge.
            allTracksEnv = try MakeMKVEnvironment.prepare(executable: makemkvcon, config: config, drive: job.drive,
                                                          home: job.logDirectory.appendingPathComponent("home-alltracks", isDirectory: true),
                                                          selectionOverride: "+sel:all")
        }
        job.appendLog("MakeMKV settings: \(home.appendingPathComponent("Library/MakeMKV/settings.conf").path)")
        if let p = env.profilePath { job.appendLog("Profile: \(p)") }

        var info = job.preloadedInfo
        if info == nil {
            if job.mode.makesMKV || job.mode == .infoOnly {
                info = try await scanDisc(job.source)
            } else {
                // Backups don't need the listing, but it tells DVD, Blu-ray and 4K UHD apart and gives the title.
                do { info = try await scanDisc(job.source) } catch {
                    if cancelRequested || error is CancellationError { throw error }
                    job.appendLog("Continuing without the disc listing: \(error.localizedDescription)", severity: .warning)
                }
            }
        }
        job.discInfo = info
        if let n = info?.name, !n.isEmpty { job.discLabel = n }
        resolveIdentity(info: info)

        let outDir = try resolveOutputDirectory()
        job.outputDirectory = outDir
        job.appendLog("Output folder: \(outDir.path)")

        switch job.mode {
        case .infoOnly:
            guard let info else { throw JobError.message("No disc information available") }
            let url = try writeDiscInfo(info, to: outDir)
            job.producedFiles.append(url)
        case .mkv:
            guard let info else { throw JobError.message("No disc information available") }
            if job.drive.rip.writeDiscInfoJSON { _ = try? writeDiscInfo(info, to: outDir) }
            try await ripTitles(source: job.source, info: info, outDir: outDir, extraSteps: 0)
        case .backup, .backupDecrypted:
            job.stepCount = 1
            let dest = try await backup(decrypt: job.mode == .backupDecrypted, outDir: outDir, inSubfolder: false)
            job.producedFiles.append(dest)
        case .backupThenMkv:
            job.stepCount = 2
            let dest = try await backup(decrypt: true, outDir: outDir, inSubfolder: true)
            job.stepIndex = 1
            let backupSource: DiscSource = job.drive.rip.backupFormat == .iso ? .iso(path: dest.path) : .folder(path: dest.path)
            let backupInfo = try await scanDisc(backupSource)
            remapSelections(from: info, to: backupInfo)
            if job.drive.rip.writeDiscInfoJSON { _ = try? writeDiscInfo(backupInfo, to: outDir) }
            try await ripTitles(source: backupSource, info: backupInfo, outDir: outDir, extraSteps: 1)
            if job.drive.rip.keepBackupAfterMKV {
                job.producedFiles.append(dest)
            } else {
                job.appendLog("Removing backup \(dest.path)")
                try? FileManager.default.removeItem(at: dest)
            }
        }
        try await archive(outDir: outDir)
    }

    // MARK: - Identity

    private func resolveIdentity(info: DiscInfo?, playAllEpisodes: Int = 0, format: DiscFormat? = nil) {
        let id = MediaIdentity.resolve(info: info, discLabel: job.discLabel, flags: job.discFlags, format: format ?? job.identity?.format,
                                       encrypted: job.mode == .backup, nameOverride: job.mediaName, kindOverride: job.mediaKind,
                                       playAllEpisodes: playAllEpisodes)
        if id != job.identity {
            let set = id.label.setDescription
            job.appendLog("Identified as \(id.kind == .tv ? "TV show" : "movie") “\(id.name)”\(set.isEmpty ? "" : " (\(set))"), \(id.format.label), format code \(id.formatCode) — \(id.reason)")
        }
        job.identity = id
    }

    // MARK: - Episodes

    /// Opens the disc's VIDEO_TS: the ISO or folder being ripped, or the mounted disc.
    private func openVideoTS(_ source: DiscSource) async -> VideoTSReader? {
        switch source {
        case .iso(let p): return ISOVideoTS(path: p)
        case .folder(let p): return FolderVideoTS(path: p, label: job.discLabel)
        case let .drive(_, dev):
            guard let mp = await EpisodeSplitter.mountPoint(device: dev) else { return nil }
            return FolderVideoTS(path: mp, label: job.discLabel)
        }
    }

    /// Decides, before ripping, which titles are episodes: a DVD "play all" title that will be split, or
    /// separate episode-length titles. Also settles movie vs. TV and the first episode number.
    private func prepareEpisodes(source: DiscSource, info: DiscInfo, indices: [Int]) async {
        episodePlan = nil
        separateEpisodes = [:]
        let ripped = indices.compactMap { info.title(at: $0) }
        var plans: [DVDNavigation.EpisodePlan] = []
        if job.identity?.format == .dvd && job.drive.episodes.splitPlayAll {
            job.phase = "Reading the disc menus"
            videoTS = await openVideoTS(source)
            if let r = videoTS, let a = DVDNavigation.analyse(r) {
                let sources = Set(ripped.compactMap(\.sourceTitleId))
                plans = DVDNavigation.plans(a).filter { sources.contains($0.title) }
                for p in plans {
                    let eps = p.starts.enumerated().map { i, c in "\(c) (\(DVDNavigation.hms(p.chapterStarts[c - 1])), \(p.reasons[i]))" }
                    job.appendLog("Disc title \(p.title): menus start \(p.episodeCount) episodes at chapters \(eps.joined(separator: ", ")); last episode ends at chapter \(p.lastEnd) (\(p.endRule))\(p.tail.isEmpty ? "" : "; chapter(s) \(p.tail.map(String.init).joined(separator: ",")) follow it"))")
                }
            } else if videoTS == nil {
                job.appendLog("Could not open the disc's VIDEO_TS to look for episodes", severity: .warning)
            }
        }
        let strict = plans.first { $0.isPlausible(strict: true) }?.episodeCount ?? 0
        resolveIdentity(info: info, playAllEpisodes: strict)
        guard job.identity?.kind == .tv else { return }

        if let p = plans.first(where: { $0.isPlausible(strict: false) }) {
            if let t = ripped.first(where: { $0.sourceTitleId == p.title }), p.matchesChapterCount(t.chapterCount) {
                episodePlan = p
            } else {
                job.appendLog("MakeMKV's title \(p.title) has a different chapter count than the disc's navigation; not splitting it", severity: .warning)
            }
        } else if !plans.isEmpty {
            job.appendLog("The menu jumps don't look like episodes (lengths \(plans[0].episodeDurations.map { DVDNavigation.hms($0) }.joined(separator: ", "))); not splitting")
        }
        if episodePlan == nil {
            for (k, t) in MediaIdentity.episodeLikeTitles(ripped).sorted(by: { $0.index < $1.index }).enumerated() { separateEpisodes[t.index] = k }
        }
        let count = episodePlan?.episodeCount ?? separateEpisodes.count
        guard count > 0 else { return }

        var first = job.firstEpisode
        var how = "entered for this disc"
        if first == nil, job.drive.episodes.readMenuNumbers, let r = videoTS {
            job.phase = "Reading episode numbers from the menus"
            let flag = cancelFlag
            let (numbers, why) = await EpisodeSplitter.ocrEpisodeNumbers(r, isCancelled: flag.isSet)
            if let numbers, let f = DVDNavigation.firstEpisode(from: numbers, count: count) {
                first = f; how = "menu text \(numbers.sorted())"
            } else {
                how = why ?? "menus read \(numbers.map { $0.sorted().description } ?? "[]"), no clear numbering"
            }
        }
        if first == nil { how = "numbered from 1 (\(how.isEmpty ? "no number given" : how))" }
        firstEpisodeNumber = first ?? 1
        episodeWidth = max(2, String(firstEpisodeNumber + count - 1).count)
        job.appendLog("Episodes \(firstEpisodeNumber)–\(firstEpisodeNumber + count - 1) (\(how))")
    }

    /// Splits the ripped "play all" title into episode files named with the file name template.
    private func splitEpisodes(outDir: URL, info: DiscInfo) async throws {
        guard let plan = episodePlan,
              let (file, titleIndex) = fileTitles.first(where: { info.title(at: $0.value)?.sourceTitleId == plan.title }).map({ ($0.key, $0.value) }),
              let title = info.title(at: titleIndex) else { return }
        guard let mkvmerge else {
            job.appendLog("Splitting episodes needs mkvmerge (MKVToolNix); kept \(file.lastPathComponent) as one file", severity: .warning)
            return
        }
        job.phase = "Splitting episodes"
        if let d = await EpisodeSplitter.duration(of: file, mkvmerge: mkvmerge), abs(d - plan.duration) > 2 {
            job.appendLog("\(file.lastPathComponent) lasts \(DVDNavigation.hms(d)), the disc title \(DVDNavigation.hms(plan.duration)); not splitting", severity: .warning)
            return
        }
        let starts = await EpisodeSplitter.chapterStarts(of: file, mkvextract: EpisodeSplitter.findTool("mkvextract", near: mkvmerge))
        guard let split = DVDNavigation.mkvChapters(for: plan.splitChapters, plan: plan, mkvStarts: starts) else {
            job.appendLog("The chapters of \(file.lastPathComponent) don't line up with the disc's episodes; not splitting", severity: .warning)
            return
        }
        guard let parts = await EpisodeSplitter.split(file, atChapters: split, mkvmerge: mkvmerge,
                                                      register: { [weak self] r in self?.activeRunner = r },
                                                      log: { [weak self] l in DispatchQueue.main.async { MainActor.assumeIsolated { self?.job.appendLog(l) } } }) else {
            if cancelRequested { throw CancellationError() }
            job.appendLog("mkvmerge could not split \(file.lastPathComponent); kept it as one file", severity: .warning)
            return
        }
        var outputs: [URL] = []
        for (i, part) in parts.enumerated() {
            var values = templateValues(status: .running)
            for (k, v) in Self.titleValues(title, ordinal: i + 1, disc: info, originalFile: part) { values[k] = v }
            let range: ClosedRange<Int>
            if i < plan.episodeCount {
                range = plan.chapterRange(i)
                values["episode"] = MediaIdentity.episodeLabel(firstEpisodeNumber + i, width: episodeWidth)
                values["episodeNumber"] = String(firstEpisodeNumber + i)
            } else {
                range = plan.tail.first!...plan.tail.last!
            }
            values["track"] = "\(MediaIdentity.trackLabel(title)) Ch \(range.lowerBound == range.upperBound ? "\(range.lowerBound)" : "\(range.lowerBound)-\(range.upperBound)")"
            let fallback = "\(file.deletingPathExtension().lastPathComponent) - \(i < plan.episodeCount ? "Episode \(firstEpisodeNumber + i)" : "after last episode")"
            let final = rename(part, values: values, template: job.drive.output.fileNameTemplate, fallbackName: fallback, outDir: outDir)
            outputs.append(final)
            if i < plan.episodeCount {
                job.episodes.append(.init(file: relativePath(final, outDir), episode: firstEpisodeNumber + i, sourceTitleId: plan.title,
                                          firstChapter: range.lowerBound, lastChapter: range.upperBound))
            }
        }
        job.appendLog("Split \(file.lastPathComponent) into \(plan.episodeCount) episode(s)\(parts.count > plan.episodeCount ? " and a closing clip" : "")")
        let at = job.producedFiles.firstIndex(of: file) ?? job.producedFiles.count
        if !job.drive.episodes.keepPlayAll {
            job.producedFiles.removeAll { $0 == file }
            try? FileManager.default.removeItem(at: file)
            job.appendLog("Removed the unsplit title \(file.lastPathComponent)")
        }
        job.producedFiles.insert(contentsOf: outputs, at: min(at + (job.drive.episodes.keepPlayAll ? 1 : 0), job.producedFiles.count))
    }

    private func relativePath(_ url: URL, _ base: URL) -> String {
        let b = base.standardizedFileURL.path, p = url.standardizedFileURL.path
        return p.hasPrefix(b + "/") ? String(p.dropFirst(b.count + 1)) : url.lastPathComponent
    }

    // MARK: - Archive

    /// Hashes every produced file and writes SHA256SUMS and bromelia.json into the output folder.
    private func archive(outDir: URL) async throws {
        let cfg = job.drive.archive
        guard cfg.checksums || cfg.archiveRecord, !job.producedFiles.isEmpty else { return }
        let files = Checksums.files(job.producedFiles, base: outDir)
        let sizes = files.map { (try? FileManager.default.attributesOfItem(atPath: $0.url.path)[.size] as? Int64) ?? 0 }
        let total = max(sizes.reduce(0, +), 1)
        job.phase = "Computing checksums"
        job.stepIndex = job.stepCount
        job.totalProgress = 0
        var entries: [Checksums.Entry] = []
        var done: Int64 = 0
        for (i, f) in files.enumerated() {
            if cancelRequested { throw CancellationError() }
            job.currentOperation = f.relative
            let base = done
            let progress = ProgressRelay { [weak self] n in
                self?.job.currentProgress = Double(n) / Double(max(sizes[i], 1))
                self?.job.totalProgress = Double(base + n) / Double(total)
            }
            let url = f.url
            let flag = cancelFlag
            let hash: String
            do {
                hash = try await Task.detached { try Checksums.sha256(of: url, progress: progress.report, isCancelled: flag.isSet) }.value
            } catch is CancellationError {
                throw CancellationError()
            } catch {
                throw JobError.message("Could not read \(f.relative) to compute its checksum: \(error.localizedDescription)")
            }
            done += sizes[i]
            entries.append(.init(path: f.relative, size: sizes[i], sha256: hash))
        }
        job.currentOperation = ""
        job.checksums = entries
        if cfg.checksums {
            let url = outDir.appendingPathComponent(Checksums.fileName)
            // Keep entries of earlier jobs that wrote to the same folder.
            var merged: [String: String] = [:]
            if let old = try? String(contentsOf: url, encoding: .utf8) { for e in Checksums.parse(old) { merged[e.path] = e.hash } }
            for e in entries { merged[e.path] = e.sha256 }
            let text = merged.keys.sorted().map { "\(merged[$0]!)  \($0)\n" }.joined()
            do {
                try text.write(to: url, atomically: true, encoding: .utf8)
                job.checksumFile = url
                job.appendLog("Wrote SHA-256 checksums of \(entries.count) file(s) to \(Checksums.fileName)")
            } catch {
                throw JobError.message("Could not write \(url.path): \(error.localizedDescription)")
            }
        }
        if cfg.archiveRecord { writeArchiveRecord(outDir: outDir) }
    }

    private func writeArchiveRecord(outDir: URL) {
        guard let id = job.identity else { return }
        let info = job.discInfo
        let titles = job.ripTitles.compactMap { info?.title(at: $0) }.map {
            ArchiveRecord.Title(index: $0.index, sourceTitleId: $0.sourceTitleId, sourceFile: $0.sourceFileName, duration: $0.durationText,
                                chapters: $0.chapterCount, sizeBytes: $0.sizeBytes, segmentMap: $0.segmentMap)
        }
        let record = ArchiveRecord(
            name: id.name, kind: id.kind.rawValue,
            disc: .init(label: job.discLabel, volumeName: info?.volumeName ?? "", type: info?.typeName ?? "", format: id.format.rawValue,
                        formatCode: id.formatCode, encrypted: id.encrypted, season: id.label.season, part: id.label.part,
                        volume: id.label.volume, disc: id.label.disc),
            rip: job.mode.makesMKV ? "Rip" : "Backup", mode: job.mode.rawValue, source: job.source.infoArgument, driveName: job.drive.name,
            makemkv: job.makemkvVersion, jobId: job.id.uuidString, startedAt: job.startedAt, finishedAt: Date(), titles: titles,
            episodes: job.episodes, files: job.checksums, warnings: job.warningCount, errors: job.errorCount, errorMessages: job.errorMessages)
        let enc = JSONEncoder()
        enc.outputFormatting = [.prettyPrinted, .sortedKeys]
        enc.dateEncodingStrategy = .iso8601
        let url = Paths.uniqueURL(outDir.appendingPathComponent("bromelia.json"))
        do {
            try enc.encode(record).write(to: url)
            job.appendLog("Wrote the archive record \(url.lastPathComponent)")
        } catch {
            job.appendLog("Could not write \(url.path): \(error.localizedDescription)", severity: .warning)
        }
    }

    // MARK: - makemkvcon invocation

    @discardableResult
    private func runMakeMKV(_ args: [String]) async throws -> RunSummary {
        if cancelRequested { throw CancellationError() }
        summary = RunSummary()
        lastTotalTitle = ""
        let runner = ProcessRunner(executable: env.executable, arguments: args, environment: env.processEnvironment,
                                   workingDirectory: env.homeDirectory)
        job.commands.append(runner.commandLine)
        job.appendLog("$ " + runner.commandLine)
        activeRunner = runner
        let showDebug = env.settings["app_ShowDebug"] == "1"
        let sink = EventSink { [weak self] ev in self?.handle(ev, showDebug: showDebug) }
        defer { activeRunner = nil }
        let out: ProcessRunner.Output
        do {
            out = try await runner.run { line in sink.receive(line) }
        } catch {
            throw JobError.message("Could not start makemkvcon: \(error.localizedDescription)")
        }
        await sink.drain()
        summary.exitCode = out.exitCode
        if cancelRequested || out.wasCancelled { throw CancellationError() }
        if let m = summary.driveMismatch { throw JobError.message(m) }
        return summary
    }

    private func handle(_ ev: RobotEvent, showDebug: Bool) {
        summary.info.consume(ev)
        switch ev {
        case .message(let m):
            let sev = m.severity
            if sev == .debug && !showDebug { return }
            job.appendLog(m.text, severity: sev)
            if sev == .error { summary.errorMessages.append(m.text); job.errorMessages.append(m.text) }
            switch m.code {
            case 1005:
                if job.makemkvVersion.isEmpty { job.makemkvVersion = m.parameters.first ?? m.text }
            case 5036, 5005:
                summary.saved = Int(m.parameters.first ?? "")
            case 5037:
                summary.saved = Int(m.parameters.first ?? "")
                summary.failed = m.parameters.count > 1 ? Int(m.parameters[1]) : nil
            default: break
            }
        case let .progressTotalTitle(_, _, name):
            job.totalOperation = name
            if name != lastTotalTitle { lastTotalTitle = name; job.appendLog("— \(name)") }
        case let .progressCurrentTitle(_, _, name):
            job.currentOperation = name
        case let .progressValue(cur, tot, mx):
            guard mx > 0 else { return }
            job.currentProgress = Double(cur) / Double(mx)
            job.totalProgress = Double(tot) / Double(mx)
        case .drive(let d):
            if let exp = expectedDrive, d.index == exp.index, !exp.device.isEmpty, !d.devicePath.isEmpty, d.devicePath != exp.device {
                summary.driveMismatch = "MakeMKV drive \(exp.index) is now \(d.devicePath), expected \(exp.device). The job was stopped to avoid reading the wrong disc; rescan drives and retry."
                activeRunner?.cancel()
            }
        case .raw(let line):
            job.appendLog(line)
        default:
            break
        }
    }

    // MARK: - Steps

    private func scanDisc(_ source: DiscSource) async throws -> DiscInfo {
        job.phase = "Reading disc information"
        let s = try await runMakeMKV(env.infoArguments(source: source, rip: job.drive.rip))
        let info = s.info.info
        if info.titles.isEmpty {
            let reason = s.errorMessages.last ?? "makemkvcon reported no titles (exit status \(s.exitCode))"
            throw JobError.message("Could not read titles from \(source.displayName): \(reason)")
        }
        job.appendLog("Found \(info.titles.count) title(s) on “\(info.name)”")
        return info
    }

    private func chooseTitles(_ info: DiscInfo) throws -> [Int] {
        if let manual = job.manualTitles {
            let valid = manual.filter { i in info.titles.contains { $0.index == i } }.sorted()
            if valid.isEmpty { throw JobError.message("None of the chosen titles exist on the disc") }
            return valid
        }
        let result = TitleSelector.evaluate(info.titles, rule: job.drive.rip.titleSelection)
        if let e = result.error { throw JobError.message(e) }
        if result.requiresManualChoice {
            throw JobError.message("“\(job.drive.name)” is set to choose titles manually. Open the disc and pick titles, or change the title rules.")
        }
        for d in result.decisions {
            let t = info.title(at: d.titleIndex)
            job.appendLog("Title \(d.titleIndex) (\(t?.durationText ?? "?"), \(t?.chapterCount ?? 0) ch): \(d.selected ? "selected" : "skipped — \(d.reason)")")
        }
        if result.selectedIndices.isEmpty { throw JobError.message("No titles matched the title selection rules") }
        return result.selectedIndices
    }

    private func ripTitles(source: DiscSource, info: DiscInfo, outDir: URL, extraSteps: Int) async throws {
        let indices = try chooseTitles(info)
        job.ripTitles = indices
        fileTitles = [:]
        await prepareEpisodes(source: source, info: info, indices: indices)
        let everyTitle = Set(indices) == Set(info.titles.map(\.index))
        let single = everyTitle && job.trackSelections.isEmpty
        let invocations = single ? ["all"] : indices.map(String.init)
        job.stepCount = extraSteps + invocations.count
        var failures: [String] = []

        for (n, t) in invocations.enumerated() {
            if cancelRequested { throw CancellationError() }
            job.stepIndex = extraSteps + n
            job.totalProgress = 0
            job.currentProgress = 0
            job.phase = single ? "Ripping \(indices.count) title(s)" : "Ripping title \(t) (\(n + 1) of \(invocations.count))"
            let before = Self.mkvFiles(in: outDir)
            env = (!single && job.trackSelections[Int(t) ?? -1] != nil) ? (allTracksEnv ?? baseEnv) : baseEnv
            defer { env = baseEnv }
            let s = try await runMakeMKV(env.mkvArguments(source: source, title: t, destination: outDir.path, rip: job.drive.rip))
            let produced = Self.mkvFiles(in: outDir).subtracting(before).sorted { $0.path < $1.path }
            if s.exitCode != 0 || (s.failed ?? 0) > 0 || produced.isEmpty {
                let why = s.errorMessages.last ?? "makemkvcon exit status \(s.exitCode)"
                failures.append(single ? why : "title \(t): \(why)")
                job.appendLog("Title \(t) failed: \(why)", severity: .error)
                if single && produced.isEmpty { break }
            }
            for file in produced {
                let titleIndex = single ? info.titles.first { $0.outputFileName == file.lastPathComponent }?.index : Int(t)
                var final = file
                if let ti = titleIndex, let keep = job.trackSelections[ti], let title = info.title(at: ti) {
                    final = await remux(final, title: title, keep: keep)
                }
                if let ti = titleIndex, let title = info.title(at: ti) {
                    final = rename(final, title: title, ordinal: (indices.firstIndex(of: ti) ?? n) + 1, info: info, outDir: outDir)
                    fileTitles[final] = ti
                }
                job.producedFiles.append(final)
            }
        }
        job.stepIndex = job.stepCount
        if !failures.isEmpty {
            throw JobError.message(failures.count == 1 ? "Rip failed: \(failures[0])" : "\(failures.count) titles failed: " + failures.joined(separator: "; "))
        }
        try await splitEpisodes(outDir: outDir, info: info)
    }

    private func backup(decrypt: Bool, outDir: URL, inSubfolder: Bool) async throws -> URL {
        guard case let .drive(index, device) = job.source else {
            throw JobError.message("Backups can only be made from a disc in a drive")
        }
        let fm = FileManager.default
        var dest = outDir
        if inSubfolder {
            let sub = TemplateRenderer.renderPath(job.drive.output.backupSubfolder, values: templateValues(status: .running))
            dest = outDir.appendingPathComponent(sub.isEmpty ? "backup" : sub, isDirectory: true)
        }
        try fm.createDirectory(at: dest, withIntermediateDirectories: true)
        let name = backupName(decrypt: decrypt)
        if job.drive.rip.backupFormat == .iso {
            dest = Paths.uniqueURL(dest.appendingPathComponent("\(name.isEmpty ? TemplateRenderer.sanitizeComponent(job.discLabel.isEmpty ? "disc" : job.discLabel) : name).iso"))
        } else if !name.isEmpty {
            dest = Paths.uniqueURL(dest.appendingPathComponent(name, isDirectory: true))
            try fm.createDirectory(at: dest, withIntermediateDirectories: true)
        }
        job.phase = decrypt ? "Backing up disc (decrypted)" : "Backing up disc"
        job.totalProgress = 0
        expectedDrive = (index, device)
        defer { expectedDrive = nil }
        guard let args = env.backupArguments(source: job.source, decrypt: decrypt, destination: dest.path, rip: job.drive.rip) else {
            throw JobError.message("Backups require a drive source")
        }
        let s = try await runMakeMKV(args)
        let exists = fm.fileExists(atPath: dest.path) && ((try? fm.contentsOfDirectory(atPath: dest.path).isEmpty == false) ?? true)
        if s.exitCode != 0 || !exists || (s.failed ?? 0) > 0 {
            throw JobError.message("Backup failed: \(s.errorMessages.last ?? "makemkvcon exit status \(s.exitCode)")")
        }
        job.appendLog("Backup saved to \(dest.path)")
        // Without a disc listing a UHD disc looks like a plain Blu-ray; the backup's index.bdmv tells them apart.
        if job.drive.rip.backupFormat == .folder, let found = DiscFormat.detect(backupFolder: dest), found != job.identity?.format {
            resolveIdentity(info: job.discInfo, format: found)
            let renamed = backupName(decrypt: decrypt)
            if !renamed.isEmpty, renamed != dest.lastPathComponent {
                let target = Paths.uniqueURL(dest.deletingLastPathComponent().appendingPathComponent(renamed, isDirectory: true))
                if (try? fm.moveItem(at: dest, to: target)) != nil {
                    job.appendLog("Renamed the backup to \(target.lastPathComponent)")
                    return target
                }
            }
        }
        return dest
    }

    /// File / folder name of a backup from the file name template (rip = Backup, no episode or track).
    private func backupName(decrypt: Bool) -> String {
        let template = job.drive.output.fileNameTemplate.trimmingCharacters(in: .whitespaces)
        guard !template.isEmpty else { return "" }
        var values = templateValues(status: .running)
        values["rip"] = "Backup"
        if let id = job.identity { values["format"] = id.format.code(encrypted: !decrypt) }
        for k in ["title", "index", "n", "source", "duration", "chapters", "original", "comment"] { values[k] = "" }
        return TemplateRenderer.renderPath(template, values: values).replacingOccurrences(of: "/", with: " - ")
    }

    private func remux(_ file: URL, title: TitleInfo, keep: Set<Int>) async -> URL {
        guard let mkvmerge else { return file }
        if keep.count == title.tracks.count { return file }
        job.phase = "Removing unselected tracks"
        guard let layout = await Remuxer.identify(mkvmerge: mkvmerge, file: file) else {
            job.appendLog("mkvmerge could not read \(file.lastPathComponent); keeping all tracks", severity: .warning)
            return file
        }
        let tmp = file.deletingLastPathComponent().appendingPathComponent(".bromelia-\(UUID().uuidString.prefix(8)).mkv")
        guard let args = Remuxer.arguments(layout: layout, title: title, keep: keep, input: file, output: tmp) else {
            job.appendLog("Track layout of \(file.lastPathComponent) differs from the disc listing; keeping all tracks", severity: .warning)
            return file
        }
        let runner = ProcessRunner(executable: mkvmerge, arguments: args)
        job.appendLog("$ " + runner.commandLine)
        activeRunner = runner
        defer { activeRunner = nil }
        let out = try? await runner.run { _ in }
        // mkvmerge exits with 1 for warnings; the output is still valid.
        if let out, out.exitCode <= 1, FileManager.default.fileExists(atPath: tmp.path) {
            do {
                _ = try FileManager.default.replaceItemAt(file, withItemAt: tmp)
                job.appendLog("Kept \(keep.count) of \(title.tracks.count) tracks in \(file.lastPathComponent)")
            } catch {
                job.appendLog("Could not replace \(file.lastPathComponent): \(error.localizedDescription)", severity: .warning)
                try? FileManager.default.removeItem(at: tmp)
            }
        } else {
            try? FileManager.default.removeItem(at: tmp)
            job.appendLog("mkvmerge failed; keeping all tracks in \(file.lastPathComponent)", severity: .warning)
        }
        return file
    }

    private func rename(_ file: URL, title: TitleInfo, ordinal: Int, info: DiscInfo, outDir: URL) -> URL {
        let explicit = job.titleNameOverrides[title.index]?.trimmingCharacters(in: .whitespaces) ?? ""
        let template = explicit.isEmpty ? job.drive.output.fileNameTemplate.trimmingCharacters(in: .whitespaces) : explicit
        var values = templateValues(status: .running)
        for (k, v) in Self.titleValues(title, ordinal: ordinal, disc: info, originalFile: file) { values[k] = v }
        values["track"] = MediaIdentity.trackLabel(title)
        if let k = separateEpisodes[title.index] {
            values["episode"] = MediaIdentity.episodeLabel(firstEpisodeNumber + k, width: episodeWidth)
            values["episodeNumber"] = String(firstEpisodeNumber + k)
        }
        let final = rename(file, values: values, template: template, fallbackName: nil, outDir: outDir)
        if let k = separateEpisodes[title.index] {
            job.episodes.append(.init(file: relativePath(final, outDir), episode: firstEpisodeNumber + k, sourceTitleId: title.sourceTitleId ?? title.index,
                                      firstChapter: 1, lastChapter: max(1, title.chapterCount)))
        }
        return final
    }

    /// Moves `file` to the name rendered from `template`. With an empty template the file keeps its
    /// name, or gets `fallbackName` (used for split episodes, whose temporary names are meaningless).
    private func rename(_ file: URL, values: [String: String], template: String, fallbackName: String?, outDir: URL) -> URL {
        var rel = template.trimmingCharacters(in: .whitespaces).isEmpty ? "" : TemplateRenderer.renderPath(template, values: values)
        if rel.isEmpty, let fallbackName { rel = TemplateRenderer.sanitizeComponent(fallbackName) }
        if rel.isEmpty { return file }
        if !rel.lowercased().hasSuffix(".mkv") { rel += ".mkv" }
        let target = Paths.uniqueURL(outDir.appendingPathComponent(rel))
        if target.standardizedFileURL == file.standardizedFileURL { return file }
        do {
            try FileManager.default.createDirectory(at: target.deletingLastPathComponent(), withIntermediateDirectories: true)
            try FileManager.default.moveItem(at: file, to: target)
            job.appendLog("Renamed \(file.lastPathComponent) → \(rel)")
            return target
        } catch {
            job.appendLog("Could not rename \(file.lastPathComponent): \(error.localizedDescription)", severity: .warning)
            return file
        }
    }

    /// Maps title choices made on the disc listing to the listing of the backup copy.
    private func remapSelections(from disc: DiscInfo?, to backup: DiscInfo) {
        guard let disc else { return }
        func key(_ t: TitleInfo) -> String { "\(t.sourceTitleId ?? -1)|\(t.durationSeconds)|\(t.segmentMap)" }
        var map: [Int: Int] = [:]
        for t in disc.titles {
            if let b = backup.titles.first(where: { key($0) == key(t) }) ?? backup.title(at: t.index) { map[t.index] = b.index }
        }
        if let manual = job.manualTitles { job.manualTitles = manual.compactMap { map[$0] } }
        var tracks: [Int: Set<Int>] = [:]
        for (k, v) in job.trackSelections { if let m = map[k] { tracks[m] = v } }
        job.trackSelections = tracks
        var names: [Int: String] = [:]
        for (k, v) in job.titleNameOverrides { if let m = map[k] { names[m] = v } }
        job.titleNameOverrides = names
    }

    // MARK: - Output

    private func resolveOutputDirectory() throws -> URL {
        let fm = FileManager.default
        let root = URL(fileURLWithPath: Paths.expandTilde(config.outputRoot(for: job.drive)), isDirectory: true)
        let rel = TemplateRenderer.renderPath(job.drive.output.folderTemplate, values: templateValues(status: .running))
        var dir = rel.isEmpty ? root : root.appendingPathComponent(rel, isDirectory: true)
        let nonEmpty = ((try? fm.contentsOfDirectory(atPath: dir.path))?.contains { !$0.hasPrefix(".") }) ?? false
        if nonEmpty {
            switch job.drive.output.conflictPolicy {
            case .uniqueSuffix: dir = Paths.uniqueURL(dir)
            case .overwrite: break
            case .skip: throw JobError.message("Output folder \(dir.path) already exists")
            }
        }
        try fm.createDirectory(at: dir, withIntermediateDirectories: true)
        return dir
    }

    private func writeDiscInfo(_ info: DiscInfo, to dir: URL) throws -> URL {
        let url = dir.appendingPathComponent("disc-info.json")
        let enc = JSONEncoder()
        enc.outputFormatting = [.prettyPrinted, .sortedKeys]
        try enc.encode(DiscInfoExport(info)).write(to: url)
        return url
    }

    static func mkvFiles(in dir: URL) -> Set<URL> {
        let items = (try? FileManager.default.contentsOfDirectory(at: dir, includingPropertiesForKeys: nil)) ?? []
        return Set(items.filter { $0.pathExtension.lowercased() == "mkv" && !$0.lastPathComponent.hasPrefix(".") })
    }

    static func titleValues(_ t: TitleInfo, ordinal: Int, disc: DiscInfo, originalFile: URL?) -> [String: String] {
        let d = t.durationSeconds
        return [
            "title": t.name.isEmpty ? disc.name : t.name,
            "index": String(t.index),
            "n": String(ordinal),
            "source": t.sourceTitleId.map(String.init) ?? "",
            "duration": String(format: "%d-%02d-%02d", d / 3600, (d / 60) % 60, d % 60),
            "chapters": String(t.chapterCount),
            "original": originalFile.map { $0.deletingPathExtension().lastPathComponent } ?? (t.outputFileName as NSString).deletingPathExtension,
            "comment": t.comment,
        ]
    }

    func templateValues(status: JobState) -> [String: String] {
        var v = TemplateRenderer.dateValues(job.startedAt ?? Date())
        let info = job.discInfo
        let discName = [info?.name, job.discLabel, info?.volumeName].compactMap { $0 }.first { !$0.isEmpty } ?? "Disc"
        v["disc"] = discName
        v["volume"] = info?.volumeName ?? job.discLabel
        v["type"] = info?.typeToken ?? "disc"
        v["drive"] = job.drive.name
        v["job"] = String(job.id.uuidString.prefix(8)).lowercased()
        if case let .drive(_, dev) = job.source { v["device"] = dev } else { v["device"] = "" }
        v["outputDir"] = job.outputDirectory?.path ?? ""
        v["status"] = status.statusWord
        v["manifest"] = job.manifestFile.path
        v["file"] = job.producedFiles.first?.path ?? ""
        v["files"] = job.producedFiles.map(\.path).joined(separator: " ")
        let id = job.identity ?? MediaIdentity.resolve(info: info, discLabel: job.discLabel, flags: job.discFlags, encrypted: job.mode == .backup,
                                                       nameOverride: job.mediaName, kindOverride: job.mediaKind)
        for (k, value) in id.templateValues(rip: job.mode.makesMKV ? "Rip" : "Backup") { v[k] = value }
        v["checksums"] = job.checksumFile?.path ?? ""
        return v
    }

    /// The drive's steps followed by the global plugins, keeping those that apply to this status and disc.
    private func applicableSteps(status: JobState) -> [PostProcessStep] {
        let id = job.identity
        return (job.drive.postProcess + config.plugins).filter {
            PostProcessor.shouldRun($0, status: status)
                && PluginMatcher.matches($0, name: id?.name ?? job.discLabel, discLabel: job.discLabel, formatCode: id?.formatCode ?? "")
        }
    }

    private func scriptEnvironment(status: JobState) -> [String: String] {
        var e: [String: String] = [
            "BROMELIA_JOB_ID": job.id.uuidString,
            "BROMELIA_STATUS": status.statusWord,
            "BROMELIA_MODE": job.mode.rawValue,
            "BROMELIA_DRIVE_NAME": job.drive.name,
            "BROMELIA_DRIVE_ID": job.drive.id.uuidString,
            "BROMELIA_DISC_NAME": job.discLabel,
            "BROMELIA_DISC_TYPE": job.discInfo?.typeToken ?? "disc",
            "BROMELIA_OUTPUT_DIR": job.outputDirectory?.path ?? "",
            "BROMELIA_FILES": job.producedFiles.map(\.path).joined(separator: "\n"),
            "BROMELIA_FILE_COUNT": String(job.producedFiles.count),
            "BROMELIA_MANIFEST": job.manifestFile.path,
            "BROMELIA_LOG": job.logFile.path,
            "BROMELIA_SOURCE": job.source.infoArgument,
        ]
        if case let .drive(_, dev) = job.source { e["BROMELIA_DEVICE"] = dev }
        if let err = job.errorMessage { e["BROMELIA_ERROR"] = err }
        if let id = job.identity {
            e["BROMELIA_NAME"] = id.name
            e["BROMELIA_KIND"] = id.kind.rawValue
            e["BROMELIA_FORMAT"] = id.formatCode
            e["BROMELIA_ENCRYPTED"] = id.encrypted ? "1" : "0"
            e["BROMELIA_SEASON"] = id.label.season.map(String.init) ?? ""
            e["BROMELIA_DISC_NUMBER"] = id.label.disc.map(String.init) ?? ""
            e["BROMELIA_DISC_SET"] = id.label.setDescription
        }
        if let c = job.checksumFile { e["BROMELIA_CHECKSUMS"] = c.path }
        return e
    }

    private func writeManifest(status: JobState) {
        let info = job.discInfo
        var titles: [JobManifest.Title] = []
        for i in job.ripTitles {
            guard let t = info?.title(at: i) else { continue }
            titles.append(.init(index: i, name: t.name, duration: t.durationText, chapters: t.chapterCount,
                                sourceTitleId: t.sourceTitleId, file: nil))
        }
        var dev = ""
        if case let .drive(_, d) = job.source { dev = d }
        let m = JobManifest(jobId: job.id.uuidString, status: status.statusWord, mode: job.mode.rawValue,
                            driveName: job.drive.name, driveId: job.drive.id.uuidString, devicePath: dev,
                            source: job.source.infoArgument, discName: job.discLabel, discType: info?.typeToken ?? "disc",
                            outputDirectory: job.outputDirectory?.path ?? "", files: job.producedFiles.map(\.path),
                            titles: titles, name: job.identity?.name ?? "", kind: job.identity?.kind.rawValue ?? "",
                            format: job.identity?.format.rawValue ?? "", formatCode: job.identity?.formatCode ?? "",
                            encrypted: job.identity?.encrypted ?? false, season: job.identity?.label.season,
                            discNumber: job.identity?.label.disc, episodes: job.episodes, checksumFile: job.checksumFile?.path,
                            checksums: job.checksums.map { .init(path: $0.path, size: $0.size, sha256: $0.sha256) },
                            startedAt: job.startedAt, finishedAt: job.finishedAt, error: job.errorMessage)
        let enc = JSONEncoder()
        enc.outputFormatting = [.prettyPrinted, .sortedKeys]
        enc.dateEncodingStrategy = .iso8601
        try? enc.encode(m).write(to: job.manifestFile)
    }
}

/// Stable JSON shape for disc-info.json (attribute names instead of numeric ids).
struct DiscInfoExport: Encodable {
    struct Track: Encodable { var index: Int; var attributes: [String: String] }
    struct Title: Encodable { var index: Int; var attributes: [String: String]; var tracks: [Track] }
    var attributes: [String: String]
    var titles: [Title]

    init(_ info: DiscInfo) {
        func named(_ a: [Int: String]) -> [String: String] {
            var o: [String: String] = [:]
            for (k, v) in a {
                let name = AttributeID(rawValue: k).map { "\($0)" } ?? "attr\(k)"
                o[name] = v
            }
            return o
        }
        attributes = named(info.attributes)
        titles = info.titles.map { t in
            Title(index: t.index, attributes: named(t.attributes), tracks: t.tracks.map { Track(index: $0.index, attributes: named($0.attributes)) })
        }
    }
}

enum Formatters {
    static func duration(_ t: TimeInterval) -> String {
        let s = Int(t.rounded())
        if s >= 3600 { return String(format: "%d:%02d:%02d", s / 3600, (s / 60) % 60, s % 60) }
        return String(format: "%d:%02d", s / 60, s % 60)
    }
}

/// Cancellation flag readable from background work (hashing).
final class CancelFlag: @unchecked Sendable {
    private let lock = NSLock()
    private var value = false
    func set() { lock.lock(); value = true; lock.unlock() }
    func isSet() -> Bool { lock.lock(); defer { lock.unlock() }; return value }
}

/// Delivers progress from a background thread to the main actor, at most ten times per second.
final class ProgressRelay: @unchecked Sendable {
    private let lock = NSLock()
    private var pending: Int64 = 0
    private var scheduled = false
    private let deliver: @MainActor (Int64) -> Void

    init(_ deliver: @escaping @MainActor (Int64) -> Void) { self.deliver = deliver }

    func report(_ value: Int64) {
        lock.lock()
        pending = value
        let schedule = !scheduled
        scheduled = true
        lock.unlock()
        guard schedule else { return }
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.1) { [self] in
            MainActor.assumeIsolated {
                lock.lock(); let v = pending; scheduled = false; lock.unlock()
                deliver(v)
            }
        }
    }
}
