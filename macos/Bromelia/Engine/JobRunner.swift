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
    private var summary = RunSummary()
    private var expectedDrive: (index: Int, device: String)?
    private var lastTotalTitle = ""

    init(job: RipJob, config: AppConfig, makemkvcon: URL, mkvmerge: URL?) {
        self.job = job
        self.config = config
        self.makemkvcon = makemkvcon
        self.mkvmerge = mkvmerge
    }

    func cancel() {
        cancelRequested = true
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
        let steps = job.drive.postProcess.filter { PostProcessor.shouldRun($0, status: status) }
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
                if let step = steps.first(where: { $0.name == r.name }), step.failJobOnError, status == .succeeded {
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
        let needInfo = job.mode.makesMKV || job.mode == .infoOnly || job.discLabel.isEmpty
        if info == nil && needInfo {
            info = try await scanDisc(job.source)
        }
        job.discInfo = info
        if let n = info?.name, !n.isEmpty { job.discLabel = n }

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
            if sev == .error { summary.errorMessages.append(m.text) }
            switch m.code {
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
                }
                job.producedFiles.append(final)
            }
        }
        job.stepIndex = job.stepCount
        if !failures.isEmpty {
            throw JobError.message(failures.count == 1 ? "Rip failed: \(failures[0])" : "\(failures.count) titles failed: " + failures.joined(separator: "; "))
        }
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
        if job.drive.rip.backupFormat == .iso {
            let name = TemplateRenderer.sanitizeComponent(job.discLabel.isEmpty ? "disc" : job.discLabel)
            dest = Paths.uniqueURL(dest.appendingPathComponent("\(name).iso"))
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
        return dest
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
        guard !template.isEmpty else { return file }
        var values = templateValues(status: .running)
        for (k, v) in Self.titleValues(title, ordinal: ordinal, disc: info, originalFile: file) { values[k] = v }
        var rel = TemplateRenderer.renderPath(template, values: values)
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
        return v
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
                            titles: titles, startedAt: job.startedAt, finishedAt: job.finishedAt, error: job.errorMessage)
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
