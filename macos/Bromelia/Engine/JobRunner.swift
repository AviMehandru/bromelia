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

/// Appends text to a file from any thread (makemkvcon's output arrives on the process reader's queue).
final class OutputFile: @unchecked Sendable {
    private let lock = NSLock()
    private let handle: FileHandle

    init?(_ url: URL) {
        if !FileManager.default.fileExists(atPath: url.path) { FileManager.default.createFile(atPath: url.path, contents: nil) }
        guard let h = try? FileHandle(forWritingTo: url) else { return nil }
        _ = try? h.seekToEnd()
        handle = h
    }

    func write(_ text: String) {
        lock.lock(); defer { lock.unlock() }
        try? handle.write(contentsOf: Data(text.utf8))
    }

    func close() {
        lock.lock(); defer { lock.unlock() }
        try? handle.close()
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
        /// First error of the run other than the "N titles saved, M failed" summary: usually the cause.
        var firstError: String?
        /// MakeMKV's warning that the output may not fit on the destination (message 5038).
        var spaceWarning: String?
        /// MakeMKV's debug log (app_ShowDebug), named by message 1004.
        var debugLog: URL?

        /// Why the run failed, for the job's error message.
        var reason: String { firstError ?? errorMessages.last ?? "makemkvcon exit status \(exitCode)" }
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
    /// Why an automatic rip stopped: the disc was archived before (see checkAlreadyArchived).
    private var alreadyArchived: String?
    private var expectedDrive: (index: Int, device: String)?
    private var lastTotalTitle = ""
    // Episodes (see prepareEpisodes)
    private var videoTS: VideoTSReader?
    private var episodePlan: DVDNavigation.EpisodePlan?
    private var separateEpisodes: [Int: Int] = [:]   // MakeMKV title → episode offset
    private var firstEpisodeNumber = 1
    private var episodeWidth = 2
    /// Episode number → title and plot, from the online lookup.
    private var episodeDetails: [Int: EpisodeDetails] = [:]
    /// The kind the online lookup searched for.
    private var lookedUpKind: MediaKind?
    /// The online match is the id chosen for this disc (not a search result).
    private var metadataChosen = false
    private var fileTitles: [URL: Int] = [:]
    // Output (see finishOutput)
    private var outputDir: URL?
    private var workDir: URL?
    /// The output folder was created for this job (not a shared folder), so it may be renamed or removed.
    private var ownsOutputDir = false
    /// The folder finishOutput moved the job's files into (the output folder, or one marked [INCOMPLETE] or [READ ERRORS]).
    private var filesFolder: URL?
    /// Everything makemkvcon printed during the job (makemkv.txt in the job folder).
    private var makemkvOutput: OutputFile?
    /// Error messages of the running makemkvcon count as read errors (rips and backups, not listings).
    private var collectDataErrors = false
    private var reportedMissingVerifier = false
    /// The main feature of a movie (the longest title ripped), for media server names.
    private var mainTitle: Int?

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
            if let skipped = alreadyArchived {
                status = .cancelled
                job.errorMessage = skipped
            } else if cancelRequested || error is CancellationError {
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
        // Name the real cause when MakeMKV itself couldn't work (expired key, LibreDrive required, …).
        if status == .failed, let p = job.makemkvProblem {
            job.errorMessage = "\(p.explanation) (\(job.errorMessage ?? "MakeMKV failed"))"
            job.appendLog(p.explanation, severity: .error)
        }
        if status == .succeeded && !job.dataErrors.isEmpty {
            status = .completedWithErrors
            job.errorMessage = "MakeMKV reported \(job.dataErrors.count) read error(s) while reading the disc, so the files may be damaged. They were kept apart from finished archives."
            job.appendLog(job.errorMessage!, severity: .error)
        }
        status = await finishOutput(status: status)
        if status == .succeeded { await writeMediaServerMetadata() }

        // Post-processing. Background steps run later, in the background queue, once the disc is out.
        writeManifest(status: status)
        let allSteps = applicableSteps(status: status)
        let steps = allSteps.filter { !$0.background }
        let backgroundSteps = allSteps.filter(\.background)
        if !steps.isEmpty && !cancelRequested && alreadyArchived == nil {
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
           ((status == .succeeded || (alreadyArchived != nil && job.drive.automation.alreadyArchived == .skip)) && job.drive.automation.ejectWhenDone)
            || ((status == .failed || status == .completedWithErrors) && job.drive.automation.ejectOnFailure) {
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
        if !backgroundSteps.isEmpty && !cancelRequested && alreadyArchived == nil {
            job.background = BackgroundWork(jobId: job.id, title: job.title, steps: backgroundSteps,
                                            context: PostProcessor.Context(status: status, values: templateValues(status: status),
                                                                           outputDirectory: job.outputDirectory, files: job.producedFiles,
                                                                           manifestPath: job.manifestFile.path, environment: scriptEnvironment(status: status)),
                                            logFile: job.logDirectory.appendingPathComponent("background.txt"))
            job.appendLog("\(backgroundSteps.count) post-processing step(s) queued to run in the background")
        }
        let summary = notificationText(status: status)
        if !config.notifications.isEmpty {
            await NotificationSender.send(config.notifications, title: summary.title, body: summary.body, status: status.statusWord) { [weak self] text in
                DispatchQueue.main.async { MainActor.assumeIsolated { self?.job.appendLog(text, severity: .warning) } }
            }
            await flushMainQueue()
        }
        job.appendLog("Finished: \(status.label) in \(Formatters.duration(job.elapsed))")
        try? job.logHandle?.close()
        job.logHandle = nil
        makemkvOutput?.close()
        makemkvOutput = nil
        // The logs go wherever the files went, including a folder marked [INCOMPLETE] or [READ ERRORS].
        if job.drive.archive.archiveRecord, let dir = filesFolder {
            for (file, name) in RipJob.archivedLogs where FileManager.default.fileExists(atPath: job.logDirectory.appendingPathComponent(file).path) {
                try? FileManager.default.copyItem(at: job.logDirectory.appendingPathComponent(file), to: Paths.uniqueURL(dir.appendingPathComponent(name)))
            }
        }

        if job.drive.automation.notify {
            Notifier.post(title: summary.title, body: summary.body, sound: job.drive.automation.playSound)
        }
        onFinished?(job)
    }

    /// Title and text of the notification for a finished job.
    private func notificationText(status: JobState) -> (title: String, body: String) {
        let body: String
        switch status {
        case .succeeded: body = "\(job.producedFiles.count) item(s) saved to \(job.outputDirectory?.path ?? "")"
        case .completedWithErrors: body = "Read errors: the files were kept in \(job.outputDirectory?.path ?? "")"
        case .cancelled: body = job.errorMessage ?? "Cancelled"
        default: body = job.errorMessage ?? "Failed"
        }
        let what = job.identity?.name ?? (job.discLabel.isEmpty ? job.sourceLabel : job.discLabel)
        return ("\(job.mode.shortLabel) \(status == .succeeded ? "finished" : status.label.lowercased()): \(what)", body)
    }

    private func flushMainQueue() async {
        await withCheckedContinuation { (c: CheckedContinuation<Void, Never>) in DispatchQueue.main.async { c.resume() } }
    }

    // MARK: - Pipeline

    private func execute() async throws {
        if case let .drive(_, device) = job.source, job.isAutomatic, job.drive.automation.waitForMountSeconds > 0, !device.isEmpty {
            job.phase = "Waiting for the disc to be mounted"
            let flag = cancelFlag
            let mounted = await DriveControl.waitForMount(device: device, seconds: job.drive.automation.waitForMountSeconds, isCancelled: flag.isSet)
            if cancelRequested { throw CancellationError() }
            job.appendLog(mounted ? "The disc is mounted" : "The disc wasn't mounted after \(job.drive.automation.waitForMountSeconds) s; continuing")
        }
        if job.mode == .audioCD || job.mode == .dataImage {
            try await executeOtherDisc()
            return
        }
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

        // The listing is always read again: titles chosen on an opened disc are only ripped when the disc
        // in the drive is still that disc and the titles can be found in the new listing.
        let opened = job.preloadedInfo
        if opened != nil { job.appendLog("Reading the disc listing again to check that the disc hasn't changed since it was opened") }
        var info: DiscInfo?
        var usingOpened = false
        if job.mode.makesMKV || job.mode == .infoOnly {
            info = try await scanDisc(job.source)
        } else {
            // Backups don't need the listing, but it tells DVD, Blu-ray and 4K UHD apart and gives the title.
            do { info = try await scanDisc(job.source) } catch {
                if cancelRequested || error is CancellationError { throw error }
                if let opened {
                    info = opened
                    usingOpened = true
                    job.appendLog("Using the listing read when the disc was opened: \(error.localizedDescription)", severity: .warning)
                } else {
                    job.appendLog("Continuing without the disc listing: \(error.localizedDescription)", severity: .warning)
                }
            }
        }
        if let opened, let fresh = info, !usingOpened {
            if let why = ListingMatcher.differentDisc(old: opened, new: fresh) {
                throw JobError.message("This is not the disc that was opened: \(why). Open the disc again.")
            }
            try applyTitleMap(from: opened, to: fresh)
        }
        job.discInfo = info
        if let n = info?.name, !n.isEmpty { job.discLabel = n }
        resolveIdentity(info: info)
        // Separate modes for DVDs, Blu-rays and 4K UHD discs (automatic and quick rips).
        if job.usesConfiguredMode {
            let m = job.drive.rip.mode(for: job.identity?.format)
            if m != job.mode {
                job.appendLog("\(job.identity?.format.label ?? "This disc"): \(m.label)")
                job.mode = m
                resolveIdentity(info: info)
            }
        }
        job.fingerprint = DiscFingerprint.of(info)
        if job.fingerprint != nil, job.mode != .infoOnly { try await checkAlreadyArchived() }
        await lookUpMetadata()

        let work = try prepareOutput()

        switch job.mode {
        case .infoOnly:
            guard let info else { throw JobError.message("No disc information available") }
            let url = try writeDiscInfo(info, to: work)
            job.producedFiles.append(url)
        case .mkv:
            guard let info else { throw JobError.message("No disc information available") }
            if job.drive.rip.writeDiscInfoJSON { _ = try? writeDiscInfo(info, to: work) }
            try await ripTitles(source: job.source, info: info, outDir: work, extraSteps: 0)
        case .backup, .backupDecrypted:
            job.stepCount = 1
            let dest = try await backup(decrypt: job.mode == .backupDecrypted, outDir: work, inSubfolder: false)
            job.producedFiles.append(dest)
        case .audioCD, .dataImage:
            break   // handled by executeOtherDisc
        case .backupThenMkv:
            job.stepCount = 2
            let dest = try await backup(decrypt: true, outDir: work, inSubfolder: true)
            job.stepIndex = 1
            let backupSource: DiscSource = Self.isDirectory(dest) ? .folder(path: dest.path) : .iso(path: dest.path)
            let backupInfo = try await scanDisc(backupSource)
            if let info { try applyTitleMap(from: info, to: backupInfo) }
            if job.drive.rip.writeDiscInfoJSON { _ = try? writeDiscInfo(backupInfo, to: work) }
            try await ripTitles(source: backupSource, info: backupInfo, outDir: work, extraSteps: 1)
            if job.drive.rip.keepBackupAfterMKV {
                job.producedFiles.append(dest)
            } else {
                job.appendLog("Removing backup \(dest.path)")
                try? FileManager.default.removeItem(at: dest)
            }
        }
    }

    /// Reserves the output folder and creates the hidden staging folder inside it, where everything is written
    /// until the job has finished and passed its checks.
    private func prepareOutput() throws -> URL {
        let outDir = try resolveOutputDirectory()
        outputDir = outDir
        job.outputDirectory = outDir
        let work = outDir.appendingPathComponent(Self.stagingPrefix + jobShortId, isDirectory: true)
        workDir = work
        try FileManager.default.createDirectory(at: work, withIntermediateDirectories: true)
        job.appendLog("Output folder: \(outDir.path) (files are moved there once the job has finished and been checked)")
        return work
    }

    /// A disc archived before (same fingerprint, in the history or a bromelia.json under the output root): automatic rips
    /// follow `automation.alreadyArchived`, manual ones only warn. Throws (with `alreadyArchived` set) to stop the job.
    private func checkAlreadyArchived() async throws {
        job.phase = "Looking for earlier archives of this disc"
        let root = URL(fileURLWithPath: Paths.expandTilde(config.outputRoot(for: job.drive)), isDirectory: true)
        let fingerprint = job.fingerprint, candidates = job.archivedCandidates
        guard let m = await Task.detached(operation: { ArchiveLookup.find(fingerprint: fingerprint, candidates: candidates, root: root) }).value
        else { return }
        let day = DateFormatter()
        day.dateFormat = "yyyy-MM-dd"
        let when = m.archivedAt.map { " on " + day.string(from: $0) } ?? ""
        let policy = job.drive.automation.alreadyArchived
        if !job.isAutomatic || policy == .ripAgain {
            job.appendLog("This disc was archived before, in \(m.folder)\(when); ripping it again", severity: .warning)
            return
        }
        let message = policy == .skip
            ? "Already archived in \(m.folder)\(when), so it wasn't ripped again. To archive it again, rip it from the drive page."
            : "Already archived in \(m.folder)\(when). Rip it again from the drive page, or eject it."
        alreadyArchived = message
        job.appendLog(message, severity: .warning)
        throw JobError.message(message)
    }

    /// The canonical title and year from TMDb or OMDb, when a provider is set up: the movie or show chosen for this disc
    /// (onlineId), else the best search result for the name (with the year typed for the disc). Failures only log.
    private func lookUpMetadata() async {
        let m = config.metadata
        guard m.provider != .none, !m.apiKey.isEmpty, let id = job.identity, !id.name.isEmpty else { return }
        lookedUpKind = id.kind
        let chosen = job.onlineId.trimmingCharacters(in: .whitespaces)
        do {
            if !chosen.isEmpty {
                if let oid = OnlineId.parse(chosen) {
                    job.phase = "Looking up \(oid.label)"
                    if let match = try await MetadataLookup.lookup(id: oid, kind: id.kind, config: m) {
                        metadataChosen = true
                        use(match, how: "chosen for this disc")
                    } else {
                        job.appendLog("\(m.provider.label) found nothing for \(oid.label), chosen for this disc", severity: .warning)
                    }
                    return
                }
                job.appendLog("“\(chosen)” is not a TMDb or IMDb id; looking up the name instead", severity: .warning)
            }
            let (query, typedYear) = MetadataLookup.splitYear(id.name)
            let year = job.mediaYear ?? typedYear
            job.phase = "Looking up “\(query)”"
            let list = try await MetadataLookup.search(name: query, kind: id.kind, year: year, config: m)
            guard let best = list.first else {
                job.appendLog("\(m.provider.label) found nothing for “\(query)”\(year.map { " (\($0))" } ?? "")", severity: .warning)
                return
            }
            let others = list.dropFirst().prefix(4).map(\.label)
            use(best, how: list.count == 1 ? "the only result" : "best of \(list.count) results; also " + others.joined(separator: ", ")
                + (list.count > 5 ? ", …" : "") + ". Choose another on the disc page if this is wrong")
        } catch {
            job.appendLog("Looking up “\(id.name)” failed: \(error.localizedDescription)", severity: .warning)
        }
    }

    private func use(_ match: MediaMatch, how: String) {
        job.metadata = match
        let ids = [match.tmdbId.map { "TMDb \($0)" }, match.imdbId].compactMap { $0 }.joined(separator: ", ")
        job.appendLog("\(match.provider): “\(match.label)”\(ids.isEmpty ? "" : ", \(ids)") (\(how))")
        resolveIdentity(info: job.discInfo)
    }

    /// Audio CDs (ripped with cyanrip / abcde) and data discs (copied to an ISO image).
    private func executeOtherDisc() async throws {
        guard case let .drive(_, device) = job.source, !device.isEmpty else {
            throw JobError.message("\(job.mode.label) needs a disc in a drive")
        }
        resolveIdentity(info: nil)
        let work = try prepareOutput()
        job.stepCount = 1
        let (exe, args): (URL, [String])
        if job.mode == .audioCD {
            guard let cmd = OtherDiscTools.audioCommand(job.drive.other, device: device) else {
                throw JobError.message("Ripping audio CDs needs cyanrip or abcde (both look up the album in MusicBrainz). Install one, or set an audio CD command.")
            }
            (exe, args) = cmd
            job.phase = "Ripping the audio CD"
        } else {
            let name = backupName(decrypt: true)
            let dest = work.appendingPathComponent("\((name.isEmpty ? "disc" : name)).iso")
            (exe, args) = OtherDiscTools.imageCommand(device: device, destination: dest)
            job.phase = "Copying the disc to an ISO image"
        }
        let runner = ProcessRunner(executable: exe, arguments: args, workingDirectory: work, stopSignal: SIGTERM)
        job.commands.append(runner.commandLine)
        job.appendLog("$ " + runner.commandLine)
        activeRunner = runner
        defer { activeRunner = nil }
        let out: ProcessRunner.Output
        do {
            out = try await runner.run(stallTimeout: TimeInterval(max(0, config.stallTimeoutMinutes) * 60)) { [weak self] line in
                DispatchQueue.main.async { MainActor.assumeIsolated { self?.job.appendLog(line) } }
            }
        } catch {
            throw JobError.message("Could not start \(exe.lastPathComponent): \(error.localizedDescription)")
        }
        await flushMainQueue()
        if cancelRequested || out.wasCancelled { throw CancellationError() }
        if out.stalled { throw JobError.message("\(exe.lastPathComponent) printed nothing for \(config.stallTimeoutMinutes) minutes and was stopped") }
        if out.exitCode != 0 { throw JobError.message("\(exe.lastPathComponent) failed (exit status \(out.exitCode))") }
        // hdiutil names its image .cdr.
        for name in Self.visibleItems(work) where name.hasSuffix(".cdr") {
            let from = work.appendingPathComponent(name)
            try? FileManager.default.moveItem(at: from, to: Paths.uniqueURL(from.deletingPathExtension().appendingPathExtension("iso")))
        }
        job.producedFiles = Self.visibleItems(work).map { work.appendingPathComponent($0) }
        if job.producedFiles.isEmpty { throw JobError.message("\(exe.lastPathComponent) saved nothing") }
        if job.mode == .dataImage, job.drive.archive.verifyRips, let iso = job.producedFiles.first(where: { $0.pathExtension == "iso" }),
           let problem = BackupVerifier.problem(iso, iso: true) {
            throw JobError.message("The disc image failed the check: \(problem)")
        }
    }

    /// Moves title and track choices made on one listing to the title numbers of another (the listing read
    /// when the job starts, or the listing of the backup). Throws when a chosen title can't be found.
    private func applyTitleMap(from old: DiscInfo, to new: DiscInfo) throws {
        let chosen = Set(job.manualTitles ?? []).union(job.trackSelections.keys).union(job.titleNameOverrides.keys)
        guard !chosen.isEmpty else { return }
        let map = try ListingMatcher.map(chosen, from: old, to: new, sameTracks: Set(job.trackSelections.keys))
        let moved = map.filter { $0.key != $0.value }.sorted { $0.key < $1.key }
        if !moved.isEmpty {
            job.appendLog("Title numbers changed: " + moved.map { "\($0.key) → \($0.value)" }.joined(separator: ", "))
        }
        if let manual = job.manualTitles { job.manualTitles = manual.compactMap { map[$0] }.sorted() }
        job.trackSelections = Dictionary(uniqueKeysWithValues: job.trackSelections.compactMap { k, v in map[k].map { ($0, v) } })
        job.titleNameOverrides = Dictionary(uniqueKeysWithValues: job.titleNameOverrides.compactMap { k, v in map[k].map { ($0, v) } })
    }

    // MARK: - Identity

    private func resolveIdentity(info: DiscInfo?, playAllEpisodes: Int = 0, format: DiscFormat? = nil) {
        var id = MediaIdentity.resolve(info: info, discLabel: job.discLabel, flags: job.discFlags, format: format ?? job.identity?.format,
                                       encrypted: job.mode == .backup, nameOverride: job.mediaName, kindOverride: job.mediaKind,
                                       playAllEpisodes: playAllEpisodes)
        if let m = job.metadata {
            id.name = TemplateRenderer.sanitizeComponent(m.title)
            // An id chosen for the disc may be a show where a movie was assumed (or the other way round).
            if metadataChosen, let k = m.kind, k != id.kind, job.mediaKind == nil {
                id.kind = k
                id.reason = "\(m.provider) lists it as a \(k == .tv ? "TV show" : "movie")"
            }
        }
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
        // The menus showed a TV show where the lookup searched for a movie: search again, with the name read from the disc.
        if let k = job.identity?.kind, let searched = lookedUpKind, k != searched, !metadataChosen {
            job.metadata = nil
            resolveIdentity(info: info, playAllEpisodes: strict)
            await lookUpMetadata()
        }
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
        if let found = await previousDisc() {
            if first == nil {
                first = found.lastEpisode + 1
                how = "after episode \(found.lastEpisode) of \(found.source)"
            } else if first != found.lastEpisode + 1 {
                job.appendLog("The previous disc ended with episode \(found.lastEpisode) (\(found.source)), but this disc starts with \(first!) (\(how))", severity: .warning)
            }
        }
        if first == nil { how = "numbered from 1 (\(how.isEmpty ? "no number given" : how))" }
        firstEpisodeNumber = first ?? 1
        episodeWidth = max(2, String(firstEpisodeNumber + count - 1).count)
        job.appendLog("Episodes \(firstEpisodeNumber)–\(firstEpisodeNumber + count - 1) (\(how))")
        await lookUpEpisodeTitles(count: count)
    }

    /// The last episode of the previous disc of this set (see EpisodeContinuation), for discs 2 and later.
    private func previousDisc() async -> EpisodeContinuation.Found? {
        guard let id = job.identity, let disc = id.label.disc, disc > 1 else { return nil }
        job.phase = "Looking for the previous disc's episodes"
        let q = EpisodeContinuation.Query(name: id.name, labelTitle: id.label.title, season: id.label.season, part: id.label.part,
                                          volume: id.label.volume, disc: disc)
        let root = URL(fileURLWithPath: Paths.expandTilde(config.outputRoot(for: job.drive)), isDirectory: true)
        let folders = job.archivedCandidates.map(\.folder)
        let season = id.label.season ?? 1
        let seasonFolder = job.drive.output.layout == .mediaServer
            ? outputDir?.appendingPathComponent(String(format: "Season %02d", season), isDirectory: true) : nil
        return await Task.detached { EpisodeContinuation.find(q, root: root, folders: folders, seasonFolder: seasonFolder, season: season) }.value
    }

    /// The titles of this disc's episodes, when the show was found online (metadata.episodeTitles). A season whose
    /// numbers don't cover every episode of the disc (numbered across seasons, say) gives no titles at all.
    private func lookUpEpisodeTitles(count: Int) async {
        episodeDetails = [:]
        let m = config.metadata
        guard m.episodeTitles, count > 0, let match = job.metadata, job.identity?.kind == .tv else { return }
        let season = job.identity?.label.season ?? 1
        let wanted = Array(firstEpisodeNumber..<(firstEpisodeNumber + count))
        job.phase = "Looking up episode titles"
        do {
            let all = try await MetadataLookup.episodes(of: match, season: season, config: m)
            guard let lo = all.keys.min(), let hi = all.keys.max() else {
                job.appendLog("\(match.provider) lists no episodes for season \(season) of “\(match.title)”", severity: .warning)
                return
            }
            let missing = wanted.filter { all[$0] == nil }
            if !missing.isEmpty {
                job.appendLog("Season \(season) of “\(match.title)” on \(match.provider) has episodes \(lo)–\(hi), not \(missing.map(String.init).joined(separator: ", ")); the episodes get no titles", severity: .warning)
                return
            }
            for n in wanted { episodeDetails[n] = all[n] }
            job.appendLog("Episode titles from \(match.provider), season \(season): " + wanted.map { "\($0) “\(all[$0]!.title)”" }.joined(separator: ", "))
        } catch {
            job.appendLog("Looking up the episode titles failed: \(error.localizedDescription)", severity: .warning)
        }
    }

    /// Template values of episode `n`.
    private func setEpisode(_ n: Int, in values: inout [String: String]) {
        values["episode"] = MediaIdentity.episodeLabel(n, width: episodeWidth)
        values["episodeNumber"] = String(n)
        values["episodeTitle"] = episodeDetails[n]?.title ?? ""
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
                setEpisode(firstEpisodeNumber + i, in: &values)
            } else {
                range = plan.tail.first!...plan.tail.last!
            }
            values["track"] = "\(MediaIdentity.trackLabel(title)) Ch \(range.lowerBound == range.upperBound ? "\(range.lowerBound)" : "\(range.lowerBound)-\(range.upperBound)")"
            let fallback = "\(file.deletingPathExtension().lastPathComponent) - \(i < plan.episodeCount ? "Episode \(firstEpisodeNumber + i)" : "after last episode")"
            let template = job.drive.output.layout == .mediaServer ? MediaServerNaming.fileTemplate(values: values, isMainFeature: false)
                : job.drive.output.fileNameTemplate
            let final = rename(part, values: values, template: template, fallbackName: fallback, outDir: outDir)
            outputs.append(final)
            if i < plan.episodeCount {
                job.episodes.append(.init(file: relativePath(final, outDir), episode: firstEpisodeNumber + i, sourceTitleId: plan.title,
                                          firstChapter: range.lowerBound, lastChapter: range.upperBound,
                                          title: episodeDetails[firstEpisodeNumber + i]?.title))
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

    // MARK: - Output folder and archive

    /// Name prefix of the hidden staging folder a job writes to inside its output folder.
    static let stagingPrefix = ".bromelia-incomplete-"

    private var jobShortId: String { String(job.id.uuidString.prefix(8)).lowercased() }

    /// Names of the entries of `dir` that aren't hidden.
    static func visibleItems(_ dir: URL) -> [String] {
        ((try? FileManager.default.contentsOfDirectory(atPath: dir.path)) ?? []).filter { !$0.hasPrefix(".") }.sorted()
    }

    /// Moves the job's files out of the staging folder. A job that succeeded goes into the output folder.
    /// Anything else goes into a folder marked “[INCOMPLETE]” (failed or cancelled) or “[READ ERRORS]”, with a
    /// note explaining why, so an unfinished or damaged rip can never look like a finished archive.
    /// Checksums and the archive record are written for complete files, including files with read errors.
    private func finishOutput(status initial: JobState) async -> JobState {
        guard let stage = workDir, let out = outputDir else { return initial }
        var status = initial
        let fm = FileManager.default
        if Self.visibleItems(stage).isEmpty {
            try? fm.removeItem(at: stage)
            // Remove the folder reserved for this job when nothing was saved.
            if status != .succeeded, ownsOutputDir,
               ((try? fm.contentsOfDirectory(atPath: out.path)) ?? ["?"]).allSatisfy({ $0 == ".DS_Store" }) {
                try? fm.removeItem(at: out)
                job.outputDirectory = nil
            }
            return status
        }

        // Hash while the files are still in the staging folder: if a file can't be read back, it stays out of the archive.
        var entries: [Checksums.Entry] = []
        let cfg = job.drive.archive
        if (status == .succeeded || status == .completedWithErrors) && (cfg.checksums || cfg.archiveRecord) && !job.producedFiles.isEmpty {
            do {
                entries = try await hashProducedFiles(base: stage)
            } catch is CancellationError {
                status = .cancelled
                job.errorMessage = "Cancelled by user"
                job.appendLog("Job cancelled", severity: .warning)
            } catch {
                status = .failed
                job.errorMessage = error.localizedDescription
                job.appendLog(error.localizedDescription, severity: .error)
            }
        }

        var dest = out
        var from = stage
        let tag: String? = status == .succeeded ? nil : (status == .completedWithErrors ? "READ ERRORS" : "INCOMPLETE")
        if let tag { (dest, from) = markedFolder(out: out, stage: stage, tag: tag) }

        job.phase = "Moving files"
        // Keys are paths relative to the staging folder; folders merged into existing ones have an entry per item.
        var moved: [String: URL] = [:]
        var left = false
        // A media server library keeps adding to the show's folders (Season 02, Other, Backup) instead of making new ones.
        let merge = tag == nil && job.drive.output.layout == .mediaServer
        func move(_ rel: String) {
            let src = from.appendingPathComponent(rel)
            let want = dest.appendingPathComponent(rel)
            var srcDir: ObjCBool = false, wantDir: ObjCBool = false
            if merge, fm.fileExists(atPath: src.path, isDirectory: &srcDir), srcDir.boolValue,
               fm.fileExists(atPath: want.path, isDirectory: &wantDir), wantDir.boolValue {
                for name in Self.visibleItems(src) { move(rel + "/" + name) }
                return
            }
            let target = Paths.uniqueURL(want)
            do {
                try fm.moveItem(at: src, to: target)
                moved[rel] = target
            } catch {
                left = true
                job.appendLog("Could not move \(rel) out of \(from.path): \(error.localizedDescription)", severity: .error)
            }
        }
        for name in Self.visibleItems(from) { move(name) }
        if left {
            if status == .succeeded {
                status = .failed
                job.errorMessage = "Some files could not be moved into the output folder; they are still in \(from.path)"
            }
        } else {
            // Only temporary files (hidden) are left.
            try? fm.removeItem(at: from)
        }

        // Paths recorded while the files were in the staging folder now point to their final place.
        let stagePath = stage.standardizedFileURL.path + "/"
        func relocate(_ u: URL) -> URL {
            let p = u.standardizedFileURL.path
            guard p.hasPrefix(stagePath) else { return u }
            let rest = String(p.dropFirst(stagePath.count))
            var key = rest
            while true {
                if let top = moved[key] {
                    return key.count == rest.count ? top : top.appendingPathComponent(String(rest.dropFirst(key.count + 1)))
                }
                guard let slash = key.lastIndex(of: "/") else { return u }
                key = String(key[..<slash])
            }
        }
        func relocate(relative r: String) -> String { relativePath(relocate(stage.appendingPathComponent(r)), dest) }
        job.producedFiles = job.producedFiles.map(relocate)
        fileTitles = Dictionary(fileTitles.map { (relocate($0.key), $0.value) }, uniquingKeysWith: { a, _ in a })
        job.episodes = job.episodes.map { var e = $0; e.file = relocate(relative: e.file); return e }
        entries = entries.map { var e = $0; e.path = relocate(relative: e.path); return e }
        job.outputDirectory = dest
        if !moved.isEmpty { filesFolder = dest }
        job.appendLog(tag == nil ? "Moved \(moved.count) item(s) into \(dest.path)" : "Kept \(moved.count) item(s) apart in \(dest.path)",
                      severity: tag == nil ? .info : .warning)

        if status == .succeeded || status == .completedWithErrors {
            if let e = writeArchive(entries, to: dest, status: status), status == .succeeded {
                status = .failed
                job.errorMessage = e
            }
        }
        if status != .succeeded { writeNote(in: dest, tag: tag ?? "INCOMPLETE", status: status) }
        return status
    }

    /// Media server layout with an online match (metadata.nfo): the movie's or show's .nfo and poster in its folder (when
    /// missing: an earlier disc may have written them, or the server rewritten them), and an .nfo next to each episode.
    /// These are not in SHA256SUMS. Failures only log.
    private func writeMediaServerMetadata() async {
        guard config.metadata.nfo, job.drive.output.layout == .mediaServer, let match = job.metadata, let dir = job.outputDirectory,
              let id = job.identity else { return }
        job.phase = "Writing media server metadata"
        let fm = FileManager.default
        var written: [String] = []
        func write(_ text: String, to url: URL) {
            guard !fm.fileExists(atPath: url.path) else { return }
            do {
                try Data(text.utf8).write(to: url, options: .withoutOverwriting)
                written.append(relativePath(url, dir))
            } catch {
                job.appendLog("Could not write \(url.path): \(error.localizedDescription)", severity: .warning)
            }
        }
        if id.kind == .tv {
            write(MediaServerMetadata.nfo(match, kind: .tv), to: dir.appendingPathComponent("tvshow.nfo"))
            let season = id.label.season ?? 1
            for e in job.episodes {
                guard let n = e.episode, let d = episodeDetails[n] else { continue }
                let file = dir.appendingPathComponent(e.file)
                write(MediaServerMetadata.episodeNfo(show: match.title, season: season, episode: n, details: d),
                      to: file.deletingPathExtension().appendingPathExtension("nfo"))
            }
        } else if let main = mainTitle, let file = fileTitles.first(where: { $0.value == main })?.key {
            write(MediaServerMetadata.nfo(match, kind: .movie), to: file.deletingPathExtension().appendingPathExtension("nfo"))
        }
        let poster = dir.appendingPathComponent(MediaServerMetadata.posterName)
        if !match.poster.isEmpty, !fm.fileExists(atPath: poster.path), let url = URL(string: match.poster) {
            do {
                try await MediaServerMetadata.download(url, to: poster)
                written.append(poster.lastPathComponent)
            } catch {
                job.appendLog("Could not download the poster from \(url.host ?? ""): \(error.localizedDescription)", severity: .warning)
            }
        }
        if !written.isEmpty {
            job.appendLog("Wrote media server metadata: " + (written.count > 4 ? written.prefix(3).joined(separator: ", ") + " and \(written.count - 3) more" : written.joined(separator: ", ")))
        }
    }

    /// Where the files of a job that didn't succeed go. When the output folder was created for this job it is
    /// renamed (“Name [INCOMPLETE]”); in a shared folder a subfolder is used (“INCOMPLETE - 1a2b3c4d”).
    /// Returns that folder and the staging folder's (possibly new) location.
    private func markedFolder(out: URL, stage: URL, tag: String) -> (URL, URL) {
        let fm = FileManager.default
        // Kept outside a media server library, where the server would pick the files up.
        if job.drive.output.layout == .mediaServer {
            let root = URL(fileURLWithPath: Paths.expandTilde(config.outputRoot(for: job.drive)), isDirectory: true)
            let name = TemplateRenderer.sanitizeComponent("\(out.lastPathComponent) [\(tag) \(jobShortId)]")
            let d = Paths.uniqueURL(root.appendingPathComponent(name, isDirectory: true))
            try? fm.createDirectory(at: d, withIntermediateDirectories: true)
            return (d, stage)
        }
        if ownsOutputDir, Self.visibleItems(out).isEmpty {
            let target = Paths.uniqueURL(out.deletingLastPathComponent().appendingPathComponent("\(out.lastPathComponent) [\(tag)]", isDirectory: true))
            do {
                try fm.moveItem(at: out, to: target)
                outputDir = target
                return (target, target.appendingPathComponent(stage.lastPathComponent, isDirectory: true))
            } catch {
                job.appendLog("Could not rename \(out.path): \(error.localizedDescription)", severity: .warning)
            }
        }
        let sub = Paths.uniqueURL(out.appendingPathComponent("\(tag) - \(jobShortId)", isDirectory: true))
        try? fm.createDirectory(at: sub, withIntermediateDirectories: true)
        return (sub, stage)
    }

    /// Explains in the folder itself why its files are not a finished archive.
    private func writeNote(in dir: URL, tag: String, status: JobState) {
        var text = "Bromelia job \(job.id.uuidString): \(status.label).\n"
        text += "These files are NOT a finished archive. Rip the disc again (clean it first if it has read errors),\n"
        text += "or check the files yourself before using them.\n\n"
        if let e = job.errorMessage { text += "\(e)\n\n" }
        if !job.dataErrors.isEmpty {
            text += "Errors reported by MakeMKV while reading the disc:\n" + job.dataErrors.map { "  \($0)\n" }.joined() + "\n"
        }
        if job.drive.archive.archiveRecord {
            let makemkv = FileManager.default.fileExists(atPath: job.makemkvLogFile.path)
            text += "The job's log is in this folder as bromelia-log.txt\(makemkv ? ", and everything MakeMKV printed as makemkv-log.txt" : "").\n"
        }
        text += "Full log: \(job.logFile.path)\n"
        try? text.write(to: dir.appendingPathComponent("\(tag).txt"), atomically: true, encoding: .utf8)
    }

    /// SHA-256 of every produced file (folders are walked), paths relative to `base`.
    private func hashProducedFiles(base: URL) async throws -> [Checksums.Entry] {
        let files = Checksums.files(job.producedFiles, base: base)
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
            let before = done
            let progress = ProgressRelay { [weak self] n in
                self?.job.currentProgress = Double(n) / Double(max(sizes[i], 1))
                self?.job.totalProgress = Double(before + n) / Double(total)
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
        return entries
    }

    /// Writes SHA256SUMS and bromelia.json into `dir`. Returns an error message when SHA256SUMS can't be written.
    private func writeArchive(_ entries: [Checksums.Entry], to dir: URL, status: JobState) -> String? {
        let cfg = job.drive.archive
        job.checksums = entries
        if cfg.checksums && !entries.isEmpty {
            let url = dir.appendingPathComponent(Checksums.fileName)
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
                job.appendLog("Could not write \(url.path): \(error.localizedDescription)", severity: .error)
                return "Could not write \(url.path): \(error.localizedDescription)"
            }
        }
        if cfg.archiveRecord { writeArchiveRecord(outDir: dir, status: status) }
        return nil
    }

    private func writeArchiveRecord(outDir: URL, status: JobState) {
        guard let id = job.identity else { return }
        let info = job.ripInfo ?? job.discInfo
        let titles = job.ripTitles.compactMap { info?.title(at: $0) }.map {
            ArchiveRecord.Title(index: $0.index, sourceTitleId: $0.sourceTitleId, sourceFile: $0.sourceFileName, duration: $0.durationText,
                                chapters: $0.chapterCount, sizeBytes: $0.sizeBytes, segmentMap: $0.segmentMap)
        }
        let record = ArchiveRecord(
            status: status.statusWord, name: id.name, kind: id.kind.rawValue,
            disc: .init(label: job.discLabel, volumeName: info?.volumeName ?? "", type: info?.typeName ?? "", format: id.format.rawValue,
                        formatCode: id.formatCode, encrypted: id.encrypted, season: id.label.season, part: id.label.part,
                        volume: id.label.volume, disc: id.label.disc, fingerprint: job.fingerprint),
            rip: job.mode.makesMKV ? "Rip" : "Backup", mode: job.mode.rawValue, source: job.source.infoArgument, driveName: job.drive.name,
            makemkv: job.makemkvVersion, jobId: job.id.uuidString, startedAt: job.startedAt, finishedAt: Date(), titles: titles,
            episodes: job.episodes, files: job.checksums, warnings: job.warningCount, errors: job.errorCount, errorMessages: job.errorMessages,
            readErrors: job.dataErrors, libreDrive: job.libreDrive)
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
    private func runMakeMKV(_ args: [String], readsData: Bool = false) async throws -> RunSummary {
        if cancelRequested { throw CancellationError() }
        summary = RunSummary()
        collectDataErrors = readsData
        defer { collectDataErrors = false }
        lastTotalTitle = ""
        // makemkvcon ignores SIGINT, so it is stopped with SIGTERM.
        let runner = ProcessRunner(executable: env.executable, arguments: args, environment: env.processEnvironment,
                                   workingDirectory: env.homeDirectory, stopSignal: SIGTERM)
        job.commands.append(runner.commandLine)
        job.appendLog("$ " + runner.commandLine)
        if makemkvOutput == nil { makemkvOutput = OutputFile(job.makemkvLogFile) }
        let raw = makemkvOutput
        raw?.write("==== \(RipJob.logTimeFormatter.string(from: Date())) $ \(runner.commandLine)\n")
        activeRunner = runner
        let showDebug = env.settings["app_ShowDebug"] == "1"
        let sink = EventSink { [weak self] ev in self?.handle(ev, showDebug: showDebug) }
        defer { activeRunner = nil }
        let out: ProcessRunner.Output
        do {
            out = try await runner.run(stallTimeout: TimeInterval(max(0, config.stallTimeoutMinutes) * 60)) { line in
                raw?.write(line + "\n")
                sink.receive(line)
            }
        } catch {
            raw?.write("==== could not start: \(error.localizedDescription)\n")
            throw JobError.message("Could not start makemkvcon: \(error.localizedDescription)")
        }
        await sink.drain()
        raw?.write("==== \(RipJob.logTimeFormatter.string(from: Date())) exit status \(out.exitCode)\n")
        keepDebugLog(summary.debugLog, command: runner.commandLine)
        summary.exitCode = out.exitCode
        // These stop makemkvcon themselves, so they are checked before cancellation.
        if !cancelRequested {
            if let m = summary.driveMismatch { throw JobError.message(m) }
            if let w = summary.spaceWarning {
                throw JobError.message("Not enough free space on the destination, so the rip was stopped before writing. MakeMKV: “\(w)”")
            }
            if out.stalled {
                throw JobError.message("makemkvcon printed nothing for \(config.stallTimeoutMinutes) minutes and was stopped\(out.abandoned ? " (it did not exit; the drive may need to be reset)" : ""). The drive or disc may be stuck: eject the disc and retry.")
            }
        }
        if cancelRequested || out.wasCancelled || out.abandoned { throw CancellationError() }
        return summary
    }

    /// MakeMKV starts its debug log afresh every time it starts, so it is added to the job's copy after each run.
    private func keepDebugLog(_ url: URL?, command: String) {
        guard let url, let data = try? Data(contentsOf: url), !data.isEmpty,
              let out = OutputFile(job.makemkvDebugLogFile) else { return }
        out.write("==== \(url.path) after $ \(command)\n")
        out.write(String(decoding: data, as: UTF8.self))
        if data.last != UInt8(ascii: "\n") { out.write("\n") }
        out.close()
    }

    private func handle(_ ev: RobotEvent, showDebug: Bool) {
        summary.info.consume(ev)
        switch ev {
        case .message(let m):
            switch MakeMKVNotice(m) {
            case .libreDrive(let d)?:
                if job.libreDrive == nil { job.libreDrive = d }
            case let n?:
                job.makemkvProblem = job.makemkvProblem ?? n
            case nil:
                break
            }
            // "Debug logging enabled, log will be saved as file:///…/MakeMKV_log.txt"
            if m.code == 1004, let p = m.parameters.first, p.hasPrefix("file://") {
                let path = String(p.dropFirst("file://".count))
                summary.debugLog = URL(fileURLWithPath: path.removingPercentEncoding ?? path)
            }
            let sev = m.severity
            if sev == .debug && !showDebug { return }
            job.appendLog(m.text, severity: sev)
            if sev == .error {
                if summary.firstError == nil && m.code != 5037 && m.code != 5004 { summary.firstError = m.text }
                summary.errorMessages.append(m.text)
                job.errorMessages.append(m.text)
                if collectDataErrors { job.dataErrors.append(m.text) }
            }
            switch m.code {
            case 1005:
                if job.makemkvVersion.isEmpty { job.makemkvVersion = m.parameters.first ?? m.text }
            case 5036, 5005:
                summary.saved = Int(m.parameters.first ?? "")
            case 5037, 5004:
                summary.saved = Int(m.parameters.first ?? "")
                summary.failed = m.parameters.count > 1 ? Int(m.parameters[1]) : nil
            case 5038:
                // "The total size of all output files may reach as much as … while there are only … free":
                // stop before anything is written rather than fail when the disk fills up.
                if summary.spaceWarning == nil {
                    summary.spaceWarning = m.text
                    activeRunner?.cancel()
                }
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

    private func ripTitles(source: DiscSource, info discInfo: DiscInfo, outDir: URL, extraSteps: Int) async throws {
        var info = discInfo
        var indices = try chooseTitles(info)
        var ripConfig = job.drive.rip
        // Rip several titles in one makemkvcon run (one read of the disc structure) when they are exactly the
        // titles above some length: then a minimum length leaves just them, and "all" rips them.
        if let pass = try await onePassListing(source: source, info: info, indices: indices) {
            info = pass.info
            indices = info.titles.map(\.index)
            ripConfig.minLengthSeconds = pass.minLength
        }
        job.ripInfo = info
        job.ripTitles = indices
        mainTitle = indices.compactMap { info.title(at: $0) }.max { $0.durationSeconds < $1.durationSeconds }?.index
        fileTitles = [:]
        await prepareEpisodes(source: source, info: info, indices: indices)
        try checkFreeSpace(titles: indices.compactMap { info.title(at: $0) }, outDir: outDir)
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
            let s = try await runMakeMKV(env.mkvArguments(source: source, title: t, destination: outDir.path, rip: ripConfig), readsData: true)
            let produced = Self.mkvFiles(in: outDir).subtracting(before).sorted { $0.path < $1.path }
            let failed = s.exitCode != 0 || (s.failed ?? 0) > 0 || produced.isEmpty
            if failed {
                let why = s.reason
                failures.append(single ? why : "title \(t): \(why)")
                job.appendLog("Title \(t) failed: \(why)", severity: .error)
                if single && produced.isEmpty { break }
            }
            for file in produced {
                let titleIndex = single ? info.titles.first { $0.outputFileName == file.lastPathComponent }?.index : Int(t)
                if failed {
                    // A file of a title that didn't finish keeps MakeMKV's name, so it can't pass for a finished one.
                    job.appendLog("Kept \(file.lastPathComponent) under MakeMKV's name: the title did not finish", severity: .warning)
                    job.producedFiles.append(file)
                    continue
                }
                if let ti = titleIndex, let title = info.title(at: ti), let problem = await verify(file, title: title) {
                    failures.append("title \(ti): \(problem)")
                    job.appendLog("Title \(ti) failed the check: \(problem)", severity: .error)
                    job.producedFiles.append(file)
                    continue
                }
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
        let mediaServer = job.drive.output.layout == .mediaServer
        if mediaServer {
            // Kept out of the media server's scan (.plexignore for Plex, .ignore for Jellyfin / Emby).
            dest = outDir.appendingPathComponent("Backup", isDirectory: true)
            try fm.createDirectory(at: dest, withIntermediateDirectories: true)
            try? "*\n".write(to: dest.appendingPathComponent(".plexignore"), atomically: true, encoding: .utf8)
            try? "".write(to: dest.appendingPathComponent(".ignore"), atomically: true, encoding: .utf8)
        } else if inSubfolder {
            let sub = TemplateRenderer.renderPath(job.drive.output.backupSubfolder, values: templateValues(status: .running))
            dest = outDir.appendingPathComponent(sub.isEmpty ? "backup" : sub, isDirectory: true)
        }
        try fm.createDirectory(at: dest, withIntermediateDirectories: true)
        var name = backupName(decrypt: decrypt)
        if name.isEmpty { name = TemplateRenderer.sanitizeComponent(job.discLabel.isEmpty ? "disc" : job.discLabel) }
        // MakeMKV refuses a folder that exists, even an empty one ("already contains a backup"), so it creates it; a
        // marker holds the name until then.
        let reserved = Paths.reserveUnique(in: dest, stem: name, ext: job.drive.rip.backupFormat == .iso ? ".iso" : "")
        dest = reserved.url
        var markers = [reserved.marker].compactMap { $0 }
        defer { markers.forEach { try? fm.removeItem(at: $0) } }
        job.phase = decrypt ? "Backing up disc (decrypted)" : "Backing up disc"
        job.totalProgress = 0
        expectedDrive = (index, device)
        defer { expectedDrive = nil }
        guard let args = env.backupArguments(source: job.source, decrypt: decrypt, destination: dest.path, rip: job.drive.rip) else {
            throw JobError.message("Backups require a drive source")
        }
        let s = try await runMakeMKV(args, readsData: true)
        let exists = fm.fileExists(atPath: dest.path) && ((try? fm.contentsOfDirectory(atPath: dest.path).isEmpty == false) ?? true)
        if s.exitCode != 0 || !exists || (s.failed ?? 0) > 0 {
            throw JobError.message("Backup failed: \(s.reason)")
        }
        // MakeMKV writes an ISO image when the destination ends in .iso. If it wrote a folder instead, the
        // backup is complete but isn't an image: keep it as a folder backup rather than failing the job.
        var isDir: ObjCBool = false
        if job.drive.rip.backupFormat == .iso, fm.fileExists(atPath: dest.path, isDirectory: &isDir), isDir.boolValue,
           BackupVerifier.problem(dest, iso: false) == nil {
            let r = Paths.reserveUnique(in: dest.deletingLastPathComponent(), stem: dest.deletingPathExtension().lastPathComponent, ext: "")
            markers += [r.marker].compactMap { $0 }
            let folder = r.url
            if (try? fm.moveItem(at: dest, to: folder)) != nil {
                job.appendLog("MakeMKV wrote a folder instead of an ISO image; kept the backup as the folder \(folder.lastPathComponent)", severity: .warning)
                dest = folder
            }
        } else if job.drive.rip.backupFormat == .folder, fm.fileExists(atPath: dest.path), !Self.isDirectory(dest),
                  !dest.lastPathComponent.lowercased().hasSuffix(".iso") {
            // MakeMKV writes DVD backups as ISO images whatever the destination is called: give the image its extension.
            let r = Paths.reserveUnique(in: dest.deletingLastPathComponent(), stem: dest.lastPathComponent, ext: ".iso")
            markers += [r.marker].compactMap { $0 }
            let image = r.url
            if (try? fm.moveItem(at: dest, to: image)) != nil {
                job.appendLog("MakeMKV wrote an ISO image (as it does for DVDs); kept the backup as \(image.lastPathComponent)")
                dest = image
            }
        }
        if job.drive.archive.verifyRips, let problem = BackupVerifier.problem(dest, iso: !Self.isDirectory(dest)) {
            throw JobError.message("Backup failed the check: \(problem)")
        }
        job.appendLog("Backup saved to \(dest.path)")
        // Without a disc listing a UHD disc looks like a plain Blu-ray; the backup's index.bdmv tells them apart.
        if Self.isDirectory(dest), let found = DiscFormat.detect(backupFolder: dest), found != job.identity?.format {
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
        let template = job.drive.output.layout == .mediaServer ? "{name}{releaseYear? ({releaseYear})} - Backup - {format}"
            : job.drive.output.fileNameTemplate.trimmingCharacters(in: .whitespaces)
        guard !template.isEmpty else { return "" }
        var values = templateValues(status: .running)
        values["rip"] = "Backup"
        if let id = job.identity { values["format"] = id.format.code(encrypted: !decrypt) }
        for k in ["title", "index", "n", "source", "duration", "chapters", "original", "comment"] { values[k] = "" }
        return TemplateRenderer.renderPath(template, values: values).replacingOccurrences(of: "/", with: " - ")
    }

    /// A listing that holds exactly `indices`, read with a minimum length that leaves out every other title, so
    /// they can be ripped in one run. Returns nil (rip title by title) when that isn't possible or worthwhile:
    /// fewer than three titles, hand-picked tracks, or a title left out that is as long as a chosen one.
    private func onePassListing(source: DiscSource, info: DiscInfo, indices: [Int]) async throws -> (info: DiscInfo, minLength: Int)? {
        guard let minLength = OnePass.minimumLength(chosen: indices, of: info, current: job.drive.rip.minLengthSeconds),
              job.trackSelections.isEmpty else { return nil }
        var rip = job.drive.rip
        rip.minLengthSeconds = minLength
        job.phase = "Reading the disc listing for a one-pass rip"
        let s = try await runMakeMKV(env.infoArguments(source: source, rip: rip))
        let listing = s.info.info
        guard OnePass.matches(listing, chosen: indices, of: info) else {
            job.appendLog("A minimum length of \(minLength) s doesn't leave exactly the chosen titles; ripping them one by one", severity: .warning)
            return nil
        }
        try applyTitleMap(from: info, to: listing)
        job.appendLog("Ripping \(indices.count) titles in one pass (a minimum title length of \(minLength) s leaves out the other \(info.titles.count - indices.count))")
        return (listing, minLength)
    }

    /// Fails the job before ripping when the destination can't hold the chosen titles (sizes from the listing),
    /// plus a copy of titles with hand-picked tracks (remuxed) and of a "play all" title (split into episodes).
    private func checkFreeSpace(titles: [TitleInfo], outDir: URL) throws {
        var need = titles.reduce(Int64(0)) { $0 + $1.sizeBytes }
        need += titles.filter { job.trackSelections[$0.index] != nil }.reduce(Int64(0)) { $0 + $1.sizeBytes }
        if let plan = episodePlan, let t = titles.first(where: { $0.sourceTitleId == plan.title }) { need += t.sizeBytes }
        guard need > 0, let free = DiskSpace.available(at: outDir) else { return }
        let required = DiskSpace.required(need)
        let fmt = { (b: Int64) in ByteCountFormatter.string(fromByteCount: b, countStyle: .file) }
        job.appendLog("Free space: \(fmt(free)); the titles need about \(fmt(need))")
        if free < required {
            throw JobError.message("Not enough free space in \(outDir.deletingLastPathComponent().path): the titles need about \(fmt(required)) (with a margin), only \(fmt(free)) is free.")
        }
    }

    /// Checks a ripped file against its title in the disc listing. Returns why it can't be trusted, or nil.
    private func verify(_ file: URL, title: TitleInfo) async -> String? {
        guard job.drive.archive.verifyRips else { return nil }
        guard let mkvmerge else {
            if !reportedMissingVerifier {
                reportedMissingVerifier = true
                job.appendLog("Ripped files can't be checked against the disc listing without mkvmerge (MKVToolNix)", severity: .warning)
            }
            return nil
        }
        job.phase = "Checking \(file.lastPathComponent)"
        guard let probe = await RipVerifier.probe(mkvmerge: mkvmerge, file: file) else {
            if cancelRequested { return nil }
            return "mkvmerge can't read \(file.lastPathComponent)"
        }
        let result = RipVerifier.check(probe, against: title)
        for n in result.notes { job.appendLog("\(file.lastPathComponent): \(n)", severity: .warning) }
        guard result.problems.isEmpty else {
            return "\(file.lastPathComponent) doesn't match the disc listing: " + result.problems.joined(separator: "; ")
        }
        let length = probe.durationSeconds.map { TitleInfo.formatDuration(Int($0.rounded())) } ?? "?"
        job.appendLog("Checked \(file.lastPathComponent): \(length), \(probe.trackTypes.count) track(s), \(probe.chapterCount) chapter(s)")
        if !title.tracks.isEmpty && probe.trackTypes.count < title.tracks.count {
            job.appendLog("\(file.lastPathComponent) has \(probe.trackTypes.count) of the \(title.tracks.count) tracks on the disc; the selection rule left out the rest (use “+sel:all” to keep every track)")
        }
        return nil
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
        var values = templateValues(status: .running)
        for (k, v) in Self.titleValues(title, ordinal: ordinal, disc: info, originalFile: file) { values[k] = v }
        values["track"] = MediaIdentity.trackLabel(title)
        if let k = separateEpisodes[title.index] { setEpisode(firstEpisodeNumber + k, in: &values) }
        let template = !explicit.isEmpty ? explicit
            : job.drive.output.layout == .mediaServer ? MediaServerNaming.fileTemplate(values: values, isMainFeature: title.index == mainTitle)
            : job.drive.output.fileNameTemplate.trimmingCharacters(in: .whitespaces)
        let final = rename(file, values: values, template: template, fallbackName: nil, outDir: outDir)
        if let k = separateEpisodes[title.index] {
            job.episodes.append(.init(file: relativePath(final, outDir), episode: firstEpisodeNumber + k, sourceTitleId: title.sourceTitleId ?? title.index,
                                      firstChapter: 1, lastChapter: max(1, title.chapterCount), title: episodeDetails[firstEpisodeNumber + k]?.title))
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

    // MARK: - Output

    private func resolveOutputDirectory() throws -> URL {
        let fm = FileManager.default
        let root = URL(fileURLWithPath: Paths.expandTilde(config.outputRoot(for: job.drive)), isDirectory: true)
        let mediaServer = job.drive.output.layout == .mediaServer
        let folderTemplate = mediaServer ? MediaServerNaming.folderTemplate : job.drive.output.folderTemplate
        let rel = TemplateRenderer.renderPath(folderTemplate, values: templateValues(status: .running))
        var dir = rel.isEmpty ? root : root.appendingPathComponent(rel, isDirectory: true)
        // A media server library keeps one folder per movie / show: later discs are added to it.
        if mediaServer {
            try fm.createDirectory(at: dir, withIntermediateDirectories: true)
            ownsOutputDir = false
            return dir
        }
        // A staging folder of a running job counts as content, so two jobs never both take a folder as new.
        let occupied = ((try? fm.contentsOfDirectory(atPath: dir.path))?.contains { !$0.hasPrefix(".") || $0.hasPrefix(Self.stagingPrefix) }) ?? false
        // Only a folder made for this disc may be renamed or removed; never the output root itself.
        var owned = !rel.isEmpty
        if occupied {
            switch job.drive.output.conflictPolicy {
            case .uniqueSuffix: dir = Paths.uniqueURL(dir)
            case .overwrite: owned = false
            case .skip: throw JobError.message("Output folder \(dir.path) already exists")
            }
        }
        try fm.createDirectory(at: dir, withIntermediateDirectories: true)
        ownsOutputDir = owned
        return dir
    }

    private func writeDiscInfo(_ info: DiscInfo, to dir: URL) throws -> URL {
        let url = dir.appendingPathComponent("disc-info.json")
        let enc = JSONEncoder()
        enc.outputFormatting = [.prettyPrinted, .sortedKeys]
        try enc.encode(DiscInfoExport(info)).write(to: url)
        return url
    }

    static func isDirectory(_ url: URL) -> Bool {
        var isDir: ObjCBool = false
        return FileManager.default.fileExists(atPath: url.path, isDirectory: &isDir) && isDir.boolValue
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
        v["releaseYear"] = job.metadata?.year.map(String.init) ?? ""
        v["tmdb"] = job.metadata?.tmdbId.map(String.init) ?? ""
        v["imdb"] = job.metadata?.imdbId ?? ""
        v["libraryFolder"] = id.kind == .tv ? "TV Shows" : "Movies"
        v["seasonOr1"] = id.label.season.map(String.init) ?? "1"
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
        let info = job.ripInfo ?? job.discInfo
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
