import Foundation
import Observation
import AppKit

/// A drive as shown in the sidebar: what MakeMKV reports, merged with its configuration.
struct DriveItem: Identifiable, Hashable {
    var entry: DriveScanEntry?
    var config: DriveConfig?
    /// Stable identity: the configuration id when configured, otherwise the device.
    var id: String
    var laneKey: String

    var displayName: String {
        if let c = config { return c.name }
        return entry.map { DriveItem.shortModel($0.driveName) } ?? "Drive"
    }

    var isConnected: Bool { entry?.isPresent ?? false }

    static func shortModel(_ name: String) -> String {
        // "BD-RE HL-DT-ST BD-RE  WH16NS60 1.02 KLAM6E84325" → "HL-DT-ST BD-RE WH16NS60"
        let parts = name.split(separator: " ").map(String.init)
        guard parts.count > 2 else { return name.isEmpty ? "Drive" : name }
        return parts.dropFirst().prefix(3).joined(separator: " ")
    }

    static func laneKey(for e: DriveScanEntry) -> String {
        e.devicePath.isEmpty ? "disc:\(e.index)" : "dev:\(e.devicePath)"
    }
}

@MainActor
@Observable
final class AppModel {
    var config: AppConfig {
        didSet { if config != oldValue { scheduleSave() } }
    }
    private(set) var scannedDrives: [DriveScanEntry] = []
    var sessions: [String: DiscSession] = [:]
    /// Ad-hoc sources opened by the user (ISO files and folders), in sidebar order.
    var fileSessionIds: [String] = []
    var jobs: [RipJob] = []
    var history: [HistoryRecord] = []
    var isScanning = false
    var lastScan: Date?
    var scanMessages: [RobotMessage] = []
    var makemkvVersion = ""
    var lastError: String?
    /// A problem with MakeMKV itself (expired key, outdated version) reported by the last makemkvcon run.
    var makemkvProblem: MakeMKVNotice?
    var selection: SidebarSelection? = .queue

    @ObservationIgnored private var runners: [UUID: JobRunner] = [:]
    @ObservationIgnored private var keepAwake: NSObjectProtocol?
    @ObservationIgnored private var saveTask: Task<Void, Never>?
    @ObservationIgnored private var pollTask: Task<Void, Never>?
    @ObservationIgnored private var tickTask: Task<Void, Never>?
    @ObservationIgnored private var watcher: OpticalMediaWatcher?
    @ObservationIgnored private var scanRunner: ProcessRunner?
    @ObservationIgnored private var knownStates: [String: DriveState] = [:]
    @ObservationIgnored private var firstScanDone = false
    @ObservationIgnored private var pendingRescan: Task<Void, Never>?

    let catalog = SettingsCatalog.load()
    /// When false (tests, previews) nothing is read from or written to disk.
    @ObservationIgnored let persistent: Bool

    init(persistent: Bool = true) {
        self.persistent = persistent
        config = persistent ? ConfigStore.load() : AppConfig()
        history = persistent ? ConfigStore.loadHistory() : []
    }

    // MARK: - Startup

    func start() {
        guard pollTask == nil else { return }
        Notifier.requestAuthorization()
        importFromMakeMKVIfFirstRun()
        watcher = OpticalMediaWatcher { [weak self] _ in self?.scheduleRescan(after: 2) }
        watcher?.start()
        pollTask = Task { [weak self] in
            await self?.refreshDrives(force: true)
            while !Task.isCancelled {
                let interval = max(3, self?.config.pollIntervalSeconds ?? 10)
                try? await Task.sleep(nanoseconds: UInt64(interval) * 1_000_000_000)
                guard let self else { return }
                if self.config.pollIntervalSeconds > 0 { await self.refreshDrives(force: false) }
            }
        }
        tickTask = Task { [weak self] in
            while !Task.isCancelled {
                try? await Task.sleep(nanoseconds: 1_000_000_000)
                self?.pump()
            }
        }
    }

    private func importFromMakeMKVIfFirstRun() {
        guard !FileManager.default.fileExists(atPath: Paths.configFile.path) else { return }
        let installed = MakeMKVEnvironment.installedSettings()
        importSettings(installed)
        if let dest = installed["app_DestinationDir"], !dest.isEmpty { config.outputRoot = dest }
        ConfigStore.save(config)
    }

    /// Copies the MakeMKV settings that Bromelia manages into the global settings.
    func importSettings(_ s: [String: String]) {
        let known = Set(catalog.allSettings.map(\.key))
        for (k, v) in s where known.contains(k) && k != "app_DataDir" {
            config.globalSettings[k] = v
        }
    }

    var makemkvcon: URL? { Paths.resolveTool(configured: config.makemkvconPath, candidates: Paths.makemkvconCandidates) }
    var mkvmerge: URL? { Paths.resolveTool(configured: config.mkvmergePath, candidates: Paths.mkvmergeCandidates) }

    // MARK: - Persistence

    private func scheduleSave() {
        guard persistent else { return }
        saveTask?.cancel()
        let snapshot = config
        saveTask = Task {
            try? await Task.sleep(nanoseconds: 400_000_000)
            if Task.isCancelled { return }
            ConfigStore.save(snapshot)
        }
    }

    func saveNow() {
        guard persistent else { return }
        saveTask?.cancel()
        ConfigStore.save(config)
        ConfigStore.saveHistory(history)
    }

    // MARK: - Drives

    var driveItems: [DriveItem] {
        var items: [DriveItem] = []
        var usedConfigs = Set<UUID>()
        for e in scannedDrives where e.isPresent {
            let c = config.driveConfig(for: e)
            if let c { usedConfigs.insert(c.id) }
            items.append(DriveItem(entry: e, config: c, id: c?.id.uuidString ?? DriveItem.laneKey(for: e), laneKey: DriveItem.laneKey(for: e)))
        }
        for c in config.drives where !usedConfigs.contains(c.id) {
            items.append(DriveItem(entry: nil, config: c, id: c.id.uuidString, laneKey: "cfg:\(c.id.uuidString)"))
        }
        return items
    }

    func driveItem(id: String) -> DriveItem? { driveItems.first { $0.id == id } }

    func scheduleRescan(after seconds: Double) {
        pendingRescan?.cancel()
        pendingRescan = Task { [weak self] in
            try? await Task.sleep(nanoseconds: UInt64(seconds * 1_000_000_000))
            if Task.isCancelled { return }
            await self?.refreshDrives(force: true)
        }
    }

    func refreshDrives(force: Bool) async {
        guard !isScanning, let exe = makemkvcon else {
            if makemkvcon == nil { lastError = "makemkvcon was not found. Install MakeMKV or set its location in Settings." }
            return
        }
        let busy = jobs.contains { $0.state == .running } || sessions.values.contains { $0.isLoading }
        if busy && !force && !config.pollWhileRipping { return }
        isScanning = true
        defer { isScanning = false; lastScan = Date() }
        do {
            let home = Paths.appSupport.appendingPathComponent("scanner", isDirectory: true)
            let env = try MakeMKVEnvironment.prepare(executable: exe, config: config, drive: config.defaultDrive, home: home)
            let collector = LineCollector()
            let runner = ProcessRunner(executable: exe, arguments: MakeMKVEnvironment.scanArguments(),
                                       environment: env.processEnvironment, workingDirectory: home)
            scanRunner = runner
            _ = try await runner.run(timeout: 180) { collector.append($0) }
            scanRunner = nil
            var entries: [DriveScanEntry] = []
            var messages: [RobotMessage] = []
            for line in collector.all {
                switch RobotParser.parse(line: line) {
                case .drive(let d)?: entries.append(d)
                case .message(let m)?:
                    if m.code == 1005, let v = m.parameters.first { makemkvVersion = v }
                    if let n = MakeMKVNotice(m), n.isLicenseProblem { makemkvProblem = n }
                    if m.severity != .debug { messages.append(m) }
                default: break
                }
            }
            scanMessages = messages.filter { $0.code != 5010 && $0.code != 5042 && $0.code != 1005 && $0.code != 1004 }
            lastError = nil
            applyScan(entries)
        } catch {
            scanRunner = nil
            lastError = "Drive scan failed: \(error.localizedDescription)"
        }
    }

    private func applyScan(_ entries: [DriveScanEntry]) {
        let present = entries.filter(\.isPresent)
        // Keep the last known state for drives that are busy with a job (MakeMKV may report them oddly).
        var merged: [DriveScanEntry] = []
        for e in present {
            let lane = DriveItem.laneKey(for: e)
            if jobs.contains(where: { $0.laneKey == lane && $0.state == .running }),
               let old = scannedDrives.first(where: { DriveItem.laneKey(for: $0) == lane }) {
                merged.append(old)
            } else {
                merged.append(e)
            }
        }
        scannedDrives = merged
        ensureSessions(merged)

        for e in merged {
            let lane = DriveItem.laneKey(for: e)
            let previous = knownStates[lane]
            knownStates[lane] = e.state
            guard firstScanDone else { continue }
            if e.state == .inserted && previous != .inserted {
                discInserted(e)
            } else if e.state != .inserted && previous == .inserted {
                sessions[lane]?.reset()
            }
        }
        firstScanDone = true
    }

    private func discInserted(_ e: DriveScanEntry) {
        let lane = DriveItem.laneKey(for: e)
        sessions[lane]?.reset()
        guard let cfg = config.driveConfig(for: e), cfg.enabled, cfg.automation.autoRipOnInsert else { return }
        guard !jobs.contains(where: { $0.laneKey == lane && !$0.state.isFinished }) else { return }
        let job = makeJob(for: e, config: cfg, mode: cfg.rip.mode)
        job.isAutomatic = true
        job.startAt = Date().addingTimeInterval(TimeInterval(max(0, cfg.automation.autoRipDelaySeconds)))
        job.state = .waiting
        job.phase = "Automatic rip"
        enqueue(job)
    }

    #if DEBUG
    func debugApplyScan(_ entries: [DriveScanEntry]) {
        applyScan(entries)
    }
    #endif

    func entry(forLane lane: String) -> DriveScanEntry? {
        scannedDrives.first { DriveItem.laneKey(for: $0) == lane }
    }

    /// Adds a configuration for a detected drive, starting from the default template.
    @discardableResult
    func configure(_ e: DriveScanEntry) -> DriveConfig {
        if let existing = config.driveConfig(for: e) { return existing }
        var c = config.defaultDrive
        c.id = UUID()
        c.postProcess = c.postProcess.map { var s = $0; s.id = UUID(); return s }
        c.name = DriveItem.shortModel(e.driveName)
        c.match = DriveMatch(driveName: e.driveName, devicePath: e.devicePath)
        config.drives.append(c)
        ensureSessions(scannedDrives)
        return c
    }

    func updateDrive(_ c: DriveConfig) {
        if c.id == config.defaultDrive.id { config.defaultDrive = c; return }
        if let i = config.drives.firstIndex(where: { $0.id == c.id }) { config.drives[i] = c }
        ensureSessions(scannedDrives)
    }

    func removeDriveConfig(_ id: UUID) {
        config.drives.removeAll { $0.id == id }
        ensureSessions(scannedDrives)
    }

    func eject(lane: String) {
        guard let e = entry(forLane: lane), !e.devicePath.isEmpty else { return }
        sessions[lane]?.reset()
        Task {
            let ok = await DiscEjector.eject(devicePath: e.devicePath)
            if !ok { self.lastError = "Could not eject \(e.devicePath)" }
            self.scheduleRescan(after: 3)
        }
    }

    // MARK: - Disc sessions

    func session(for item: DriveItem) -> DiscSession? {
        item.entry == nil ? nil : sessions[item.laneKey]
    }

    /// Makes sure every connected drive has a disc session (created outside of view updates).
    private func ensureSessions(_ entries: [DriveScanEntry]) {
        for e in entries {
            let lane = DriveItem.laneKey(for: e)
            let cfgId = config.driveConfig(for: e)?.id ?? config.defaultDrive.id
            if let s = sessions[lane] {
                s.source = .drive(index: e.index, devicePath: e.devicePath)
                if s.configId != cfgId { s.configId = cfgId }
                if s.discFlags != e.flags { s.discFlags = e.flags }
            } else {
                let s = DiscSession(id: lane, source: .drive(index: e.index, devicePath: e.devicePath), configId: cfgId)
                s.discFlags = e.flags
                sessions[lane] = s
            }
        }
    }

    func openFileSource(_ url: URL) {
        let isDir = (try? url.resourceValues(forKeys: [.isDirectoryKey]).isDirectory) ?? false
        // A disc image, a disc folder, or the disc folder a file (.IFO, .mpls, .m2ts, …) belongs to.
        let source = SourceResolver.source(for: url, isDirectory: isDir)
        let key = source.infoArgument
        if sessions[key] == nil {
            let s = DiscSession(id: key, source: source, configId: config.defaultDrive.id)
            if case .folder(let p) = source, p != url.path {
                s.appendLog("Opening the disc that \(url.lastPathComponent) belongs to: \(p)", severity: .info)
            }
            sessions[key] = s
            fileSessionIds.append(key)
        }
        selection = .source(key)
        NSDocumentController.shared.noteNewRecentDocumentURL(url)
        Task { await loadDisc(sessions[key]!) }
    }

    func closeFileSource(_ key: String) {
        sessions[key]?.reset()
        sessions[key] = nil
        fileSessionIds.removeAll { $0 == key }
        if selection == .source(key) { selection = .queue }
    }

    func configForSession(_ s: DiscSession) -> DriveConfig {
        config.driveConfig(id: s.configId) ?? config.defaultDrive
    }

    /// Reads the title listing of a disc (MakeMKV's "Open disc").
    func loadDisc(_ s: DiscSession) async {
        guard !s.isLoading else { return }
        guard let exe = makemkvcon else { s.loadError = "makemkvcon not found"; return }
        if case .drive = s.source, jobs.contains(where: { $0.laneKey == s.id && $0.state == .running }) {
            s.loadError = "The drive is busy with a job."
            return
        }
        s.reset()
        s.isLoading = true
        s.operation = "Opening disc"
        let cfg = configForSession(s)
        do {
            let home = Paths.appSupport.appendingPathComponent("sessions/\(abs(s.id.hashValue))", isDirectory: true)
            let env = try MakeMKVEnvironment.prepare(executable: exe, config: config, drive: cfg, home: home)
            let args = env.infoArguments(source: s.source, rip: cfg.rip)
            let runner = ProcessRunner(executable: exe, arguments: args, environment: env.processEnvironment, workingDirectory: home)
            s.runner = runner
            s.appendLog("$ " + runner.commandLine, severity: .info)
            var builder = DiscInfoBuilder()
            let showDebug = env.settings["app_ShowDebug"] == "1"
            var errors: [String] = []
            let sink = EventSink { [weak s, weak self] ev in
                guard let s else { return }
                builder.consume(ev)
                switch ev {
                case .message(let m):
                    switch MakeMKVNotice(m) {
                    case .libreDrive(let d)?: s.libreDrive = .enabled(d)
                    case .libreDriveRequired?: s.libreDrive = .required
                    case let n? where n.isLicenseProblem: self?.makemkvProblem = n
                    default: break
                    }
                    if m.severity == .debug && !showDebug { break }
                    s.appendLog(m.text, severity: m.severity)
                    if m.severity == .error { errors.append(m.text) }
                case let .progressCurrentTitle(_, _, name): s.operation = name
                case let .progressValue(_, tot, mx): if mx > 0 { s.progress = Double(tot) / Double(mx) }
                case .raw(let l): s.appendLog(l, severity: .info)
                default: break
                }
            }
            let out = try await runner.run { sink.receive($0) }
            await sink.drain()
            s.runner = nil
            s.isLoading = false
            if case .drive = s.source, s.libreDrive == nil { s.libreDrive = .notInUse }
            if out.wasCancelled { s.loadError = "Cancelled"; return }
            if builder.info.titles.isEmpty {
                if let p = makemkvProblem, p.isLicenseProblem {
                    s.loadError = p.explanation
                } else if case .folder = s.source, errors.isEmpty {
                    s.loadError = "MakeMKV found no titles here. It opens disc images and disc folders (with BDMV, VIDEO_TS or HVDVD_TS), not single video files."
                } else {
                    s.loadError = errors.last ?? "No titles found (exit status \(out.exitCode))."
                }
                return
            }
            s.info = builder.info
            s.applyRule(cfg.rip.titleSelection)
        } catch {
            s.isLoading = false
            s.loadError = error.localizedDescription
        }
    }

    // MARK: - Jobs

    func makeJob(for e: DriveScanEntry, config cfg: DriveConfig, mode: RipMode) -> RipJob {
        let job = RipJob(source: .drive(index: e.index, devicePath: e.devicePath), drive: cfg, laneKey: DriveItem.laneKey(for: e),
                         sourceLabel: cfg.name, discLabel: e.discName, mode: mode)
        job.discFlags = e.flags
        return job
    }

    /// Queues a job for a drive using its configured mode and rules (no disc listing required).
    func quickRip(_ item: DriveItem, mode: RipMode? = nil) {
        guard let e = item.entry else { return }
        let cfg = item.config ?? config.defaultDrive
        let job = makeJob(for: e, config: cfg, mode: mode ?? cfg.rip.mode)
        if let s = sessions[item.laneKey], let info = s.info {
            job.preloadedInfo = info
            job.discLabel = info.name
            applyIdentityChoices(from: s, to: job)
        }
        enqueue(job)
    }

    /// Queues a job from an opened disc using the titles / tracks chosen in the UI.
    func ripSession(_ s: DiscSession, mode: RipMode, useChosenTitles: Bool = true) {
        let cfg = configForSession(s)
        var job: RipJob
        switch s.source {
        case let .drive(index, dev):
            let label = (config.drives.first { $0.id == s.configId }?.name) ?? cfg.name
            job = RipJob(source: .drive(index: index, devicePath: dev), drive: cfg, laneKey: s.id, sourceLabel: label,
                         discLabel: s.info?.name ?? "", mode: mode)
        case .iso, .folder:
            job = RipJob(source: s.source, drive: cfg, laneKey: s.id, sourceLabel: s.source.displayName,
                         discLabel: s.info?.name ?? "", mode: mode)
        }
        job.preloadedInfo = s.info
        applyIdentityChoices(from: s, to: job)
        if case .drive = s.source, job.discFlags == nil { job.discFlags = s.discFlags }
        if useChosenTitles && mode.makesMKV {
            job.manualTitles = s.selectedTitles.sorted()
            job.trackSelections = s.trackSelections.filter { s.selectedTitles.contains($0.key) }
            job.titleNameOverrides = s.titleNameOverrides.filter { s.selectedTitles.contains($0.key) && !$0.value.trimmingCharacters(in: .whitespaces).isEmpty }
        }
        let folder = s.outputFolderOverride.trimmingCharacters(in: .whitespaces)
        if !folder.isEmpty {
            job.drive.output.rootOverride = folder
            job.drive.output.folderTemplate = ""
            job.drive.output.conflictPolicy = .overwrite
        }
        enqueue(job)
    }

    private func applyIdentityChoices(from s: DiscSession, to job: RipJob) {
        job.mediaName = s.mediaName.trimmingCharacters(in: .whitespaces)
        job.mediaKind = s.mediaKind
        job.firstEpisode = s.firstEpisode
        if job.discFlags == nil { job.discFlags = s.discFlags }
    }

    func enqueue(_ job: RipJob) {
        jobs.append(job)
        pump()
    }

    func cancel(_ job: RipJob) {
        if let r = runners[job.id] {
            r.cancel()
        } else if !job.state.isFinished {
            job.state = .cancelled
            job.phase = "Cancelled"
            job.finishedAt = Date()
            recordHistory(job)
        }
    }

    /// Starts a waiting (delayed) job immediately.
    func startNow(_ job: RipJob) {
        guard job.state == .waiting else { return }
        job.startAt = nil
        job.state = .queued
        pump()
    }

    func retry(_ job: RipJob) {
        let j = RipJob(source: job.source, drive: job.drive, laneKey: job.laneKey, sourceLabel: job.sourceLabel,
                       discLabel: job.discLabel, mode: job.mode)
        j.preloadedInfo = job.preloadedInfo
        j.manualTitles = job.manualTitles
        j.trackSelections = job.trackSelections
        j.titleNameOverrides = job.titleNameOverrides
        j.mediaName = job.mediaName
        j.mediaKind = job.mediaKind
        j.firstEpisode = job.firstEpisode
        j.discFlags = job.discFlags
        enqueue(j)
    }

    func clearFinishedJobs() {
        jobs.removeAll { $0.state.isFinished }
    }

    func moveJob(_ job: RipJob, by offset: Int) {
        guard let i = jobs.firstIndex(where: { $0.id == job.id }) else { return }
        let j = i + offset
        guard j >= 0, j < jobs.count else { return }
        jobs.swapAt(i, j)
    }

    var activeJobCount: Int { jobs.filter { $0.state == .running }.count }

    func activeJob(lane: String) -> RipJob? {
        jobs.first { $0.laneKey == lane && ($0.state == .running || $0.state == .waiting || $0.state == .queued) }
    }

    /// Starts queued jobs whose lane is free.
    func pump() {
        let now = Date()
        for job in jobs where job.state == .waiting {
            if let t = job.startAt, t <= now { job.state = .queued }
        }
        for job in jobs where job.state == .queued {
            if config.maxConcurrentJobs > 0 && activeJobCount >= config.maxConcurrentJobs { break }
            if jobs.contains(where: { $0.laneKey == job.laneKey && $0.state == .running }) { continue }
            if let s = sessions[job.laneKey], s.isLoading { continue }
            start(job)
        }
    }

    private func start(_ job: RipJob) {
        guard let exe = makemkvcon else {
            job.state = .failed
            job.errorMessage = "makemkvcon not found"
            job.finishedAt = Date()
            recordHistory(job)
            return
        }
        // Drive numbering may have changed since the job was queued; refresh the index from the last scan.
        if case let .drive(_, dev) = job.source, !dev.isEmpty, let e = scannedDrives.first(where: { $0.devicePath == dev }) {
            job.source = .drive(index: e.index, devicePath: dev)
        }
        let runner = JobRunner(job: job, config: config, makemkvcon: exe, mkvmerge: mkvmerge)
        runner.onFinished = { [weak self] finished in
            guard let self else { return }
            self.runners[finished.id] = nil
            self.updateKeepAwake()
            if let p = finished.makemkvProblem, p.isLicenseProblem { self.makemkvProblem = p }
            self.recordHistory(finished)
            self.scheduleRescan(after: 3)
            self.pump()
        }
        runners[job.id] = runner
        updateKeepAwake()
        Task { await runner.run() }
    }

    /// Keeps the Mac awake (no idle sleep, no App Nap) while any job runs.
    func updateKeepAwake() {
        let busy = !runners.isEmpty && config.preventSleep
        if busy, keepAwake == nil {
            keepAwake = ProcessInfo.processInfo.beginActivity(options: [.userInitiated, .idleSystemSleepDisabled], reason: "Ripping discs")
        } else if !busy, let a = keepAwake {
            ProcessInfo.processInfo.endActivity(a)
            keepAwake = nil
        }
    }

    private func recordHistory(_ job: RipJob) {
        let rec = HistoryRecord(id: job.id, title: job.title, driveName: job.drive.name, discName: job.discLabel, mode: job.mode,
                                state: job.state, startedAt: job.startedAt, finishedAt: job.finishedAt,
                                outputDirectory: job.outputDirectory?.path, files: job.producedFiles.map(\.path),
                                errorMessage: job.errorMessage, logPath: job.logFile.path,
                                warnings: job.warningCount, errors: job.errorCount)
        history.removeAll { $0.id == rec.id }
        history.insert(rec, at: 0)
        guard persistent else { return }
        if history.count > config.historyLimit {
            for old in history[config.historyLimit...] {
                try? FileManager.default.removeItem(at: URL(fileURLWithPath: old.logPath).deletingLastPathComponent())
            }
            history = Array(history.prefix(config.historyLimit))
        }
        ConfigStore.saveHistory(history)
    }

    func clearHistory() {
        for h in history { try? FileManager.default.removeItem(at: URL(fileURLWithPath: h.logPath).deletingLastPathComponent()) }
        history = []
        ConfigStore.saveHistory(history)
    }

    // MARK: - Presets

    func savePreset(name: String, from drive: DriveConfig) {
        var body = drive
        body.match = DriveMatch()
        config.presets.append(DrivePreset(name: name, config: body))
    }

    func applyPreset(_ preset: DrivePreset, to drive: DriveConfig) -> DriveConfig {
        var c = drive.applyingBody(of: preset.config)
        c.postProcess = c.postProcess.map { var s = $0; s.id = UUID(); return s }
        return c
    }

    // MARK: - Registration

    /// Downloads the current beta key from the MakeMKV forum and registers it with MakeMKV.
    func installBetaKey() async -> String {
        let key: String
        do { key = try await BetaKey.fetch() } catch { return "Could not get the beta key: \(error.localizedDescription)" }
        let result = await registerWithMakeMKV(key: key)
        // A key typed into Bromelia would override the new one for every drive.
        if !config.registrationKey.isEmpty { config.registrationKey = key }
        if result.lowercased().contains("fail") { return result }
        makemkvProblem = nil
        return "Registered the current beta key (\(key.prefix(8))…). \(result)"
    }

    func registerWithMakeMKV(key: String) async -> String {
        guard let exe = makemkvcon else { return "makemkvcon not found" }
        let collector = LineCollector()
        let runner = ProcessRunner(executable: exe, arguments: ["reg", key])
        do {
            let out = try await runner.run(timeout: 60) { collector.append($0) }
            let text = collector.all.compactMap { line -> String? in
                if case .message(let m)? = RobotParser.parse(line: line) { return m.text }
                return line
            }.joined(separator: "\n")
            return out.exitCode == 0 ? (text.isEmpty ? "Key registered." : text) : "Registration failed: \(text)"
        } catch {
            return error.localizedDescription
        }
    }
}

enum SidebarSelection: Hashable {
    case drive(String)
    case source(String)
    case queue
    case history
}
