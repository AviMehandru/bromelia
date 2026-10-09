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
        didSet {
            if config != oldValue { scheduleSave() }
            if config.webUI != oldValue.webUI { web.apply(config.webUI) }
            background.limit = config.backgroundJobs
            if config.preventSleep != oldValue.preventSleep { updateKeepAwake() }
        }
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
    let background = BackgroundQueue()
    @ObservationIgnored private(set) lazy var web = WebServer(model: self)
    @ObservationIgnored private var betaKeyTried = false
    @ObservationIgnored private var keepAwake: NSObjectProtocol?
    /// Turns sleep prevention on and off (replaced in tests).
    @ObservationIgnored var keepAwakeSwitch: (Bool) -> Void = { _ in }
    @ObservationIgnored private(set) var isKeepingAwake = false
    @ObservationIgnored private var saveTask: Task<Void, Never>?
    @ObservationIgnored private var pollTask: Task<Void, Never>?
    @ObservationIgnored private var tickTask: Task<Void, Never>?
    @ObservationIgnored private var watcher: OpticalMediaWatcher?
    @ObservationIgnored private var scanRunner: ProcessRunner?
    @ObservationIgnored private var knownStates: [String: DriveState] = [:]
    @ObservationIgnored private var firstScanDone = false
    /// Drives asked to close their tray, and when: the first scan 4 s later tells whether the tray moved.
    @ObservationIgnored private var trayCloseRequests: [String: Date] = [:]
    @ObservationIgnored private var pendingRescan: Task<Void, Never>?

    let catalog = SettingsCatalog.load()
    /// When false (tests, previews) nothing is read from or written to disk.
    @ObservationIgnored let persistent: Bool
    /// Running without a window (`--headless`).
    @ObservationIgnored let headless: Bool
    /// Written to the configuration file instead of `config.webUI` (web settings from the headless command line).
    @ObservationIgnored var savedWebUI: WebUIConfig?
    /// Whether this process rips inserted discs and runs the scheduled archive check (see AutomationLock).
    @ObservationIgnored let automationLock = AutomationLock()
    private(set) var ownsAutomation = false
    @ObservationIgnored private var nextAutomationTry = Date.distantFuture

    init(persistent: Bool = true, headless: Bool = false) {
        self.persistent = persistent
        self.headless = headless
        // Without files (tests) there is no other process to share the drives with.
        ownsAutomation = !persistent
        config = persistent ? ConfigStore.load() : AppConfig()
        history = persistent ? ConfigStore.loadHistory() : []
        keepAwakeSwitch = { [weak self] on in self?.setProcessActivity(on) }
        background.onChange = { [weak self] in self?.updateKeepAwake() }
        if persistent {
            recoverUnfinishedJobs()
            checkRecords = CheckRecords.load()
        }
    }

    // MARK: - Startup

    func start() {
        guard pollTask == nil else { return }
        takeAutomation()
        if !ownsAutomation && !headless {
            lastError = "Bromelia is also running in the background (\(automationLock.holder)): it rips inserted discs and runs the scheduled archive check, so this window doesn't."
        }
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
        // The first scheduled archive check (archiveCheck.intervalDays) a minute after starting, then hourly.
        nextScheduleCheck = Date().addingTimeInterval(60)
        tickTask = Task { [weak self] in
            while !Task.isCancelled {
                try? await Task.sleep(nanoseconds: 1_000_000_000)
                self?.pump()
                self?.takeAutomationIfDue()
                self?.startScheduledCheckIfDue()
            }
        }
        background.limit = config.backgroundJobs
        web.apply(config.webUI)
        if config.autoUpdateBetaKey { Task { await updateBetaKeyIfNeeded(reason: "at startup") } }
    }

    /// With `autoUpdateBetaKey`: registers the forum's current beta key when MakeMKV uses a beta key (or none)
    /// and it differs. A purchased key is never replaced.
    @discardableResult
    func updateBetaKeyIfNeeded(reason: String) async -> String? {
        let installed = config.registrationKey.isEmpty ? MakeMKVEnvironment.installedRegistrationKey() : config.registrationKey
        guard BetaKey.mayReplace(installed) else { return nil }
        guard let key = try? await BetaKey.fetch(), key != installed else { return nil }
        let result = await installBetaKey()
        lastError = "Beta key updated \(reason): \(result)"
        return result
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

    /// Takes the automation lock when no other Bromelia holds it.
    private func takeAutomation() {
        guard persistent else { return }
        ownsAutomation = automationLock.acquire(who: headless ? "headless" : "the app")
        nextAutomationTry = Date().addingTimeInterval(60)
    }

    private func takeAutomationIfDue() {
        guard !ownsAutomation, Date() >= nextAutomationTry else { return }
        takeAutomation()
        if ownsAutomation && lastError?.hasPrefix("Bromelia is also running in the background") == true { lastError = nil }
    }

    /// The configuration as it is saved (the file keeps its own web settings when the command line changed them).
    private var configToSave: AppConfig {
        var c = config
        if let w = savedWebUI { c.webUI = w }
        return c
    }

    private func scheduleSave() {
        guard persistent else { return }
        saveTask?.cancel()
        let snapshot = configToSave
        saveTask = Task {
            try? await Task.sleep(nanoseconds: 400_000_000)
            if Task.isCancelled { return }
            ConfigStore.save(snapshot)
        }
    }

    func saveNow() {
        guard persistent else { return }
        saveTask?.cancel()
        ConfigStore.save(configToSave)
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
        checkTrayCloseRequests(merged)

        // A lane that is no longer listed lost its disc: on macOS an empty drive has no device path, so it is listed
        // as disc:N until the next disc comes (often under the same /dev/rdiskN), which must count as inserted again.
        let lanes = Set(merged.map(DriveItem.laneKey(for:)))
        for (lane, state) in knownStates where !lanes.contains(lane) {
            if state == .inserted { sessions[lane]?.reset() }
            knownStates[lane] = nil
        }

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
        // Drives without their own configuration use the default one, as everywhere else.
        let cfg = config.driveConfig(for: e) ?? config.defaultDrive
        guard ownsAutomation, cfg.enabled, cfg.automation.autoRipOnInsert else { return }
        guard !jobs.contains(where: { $0.laneKey == lane && !$0.state.isFinished }) else { return }
        guard let mode = DiscContent.mode(flags: e.flags, content: { DiscContentProbe.probe(device: e.devicePath) }, drive: cfg) else { return }
        let job = makeJob(for: e, config: cfg, mode: mode)
        job.usesConfiguredMode = mode == cfg.rip.mode
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

    func closeTray(lane: String) {
        guard let e = entry(forLane: lane) else { return }
        expectTrayClosed(driveNames: [e.driveName])
        Task {
            if !(await DriveControl.closeTray(driveName: e.driveName)) { self.lastError = "Could not close the tray of \(DriveItem.shortModel(e.driveName))" }
            self.scheduleRescan(after: 5)
        }
    }

    func closeAllTrays() {
        expectTrayClosed(driveNames: scannedDrives.filter { $0.isPresent && $0.state != .inserted }.map(\.driveName))
        Task {
            if !(await DriveControl.closeTray(driveName: "")) { self.lastError = "Could not close the trays" }
            self.scheduleRescan(after: 5)
        }
    }

    /// Checks, in the first scan at least 4 s after `date`, that these drives' trays closed. A drive without a tray motor
    /// (most slim drives) accepts the command and leaves the tray open, and the OS can't tell; MakeMKV's scan can.
    func expectTrayClosed(driveNames: [String], at date: Date = Date()) {
        for n in driveNames { trayCloseRequests[n] = date }
    }

    static func trayStillOpen(_ driveName: String) -> String {
        "The tray of \(DriveItem.shortModel(driveName)) is still open: this drive can't close it by itself (slim drives have no tray motor), so push it in by hand"
    }

    private func checkTrayCloseRequests(_ entries: [DriveScanEntry], now: Date = Date()) {
        for (name, asked) in trayCloseRequests where now.timeIntervalSince(asked) >= 4 {
            trayCloseRequests[name] = nil
            if let e = entries.first(where: { $0.driveName == name }), e.state == .emptyOpen { lastError = Self.trayStillOpen(name) }
        }
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
            lookUp(s)
        } catch {
            s.isLoading = false
            s.loadError = error.localizedDescription
        }
    }

    /// Looks the opened disc up online (with the name, kind and year chosen on the disc page) and keeps the candidates,
    /// so the disc page can show them.
    func lookUp(_ s: DiscSession) {
        let m = config.metadata
        guard m.provider != .none, !m.apiKey.trimmingCharacters(in: .whitespaces).isEmpty, let info = s.info else { return }
        let id = MediaIdentity.resolve(info: info, discLabel: info.name, flags: s.discFlags, encrypted: false,
                                       nameOverride: s.mediaName, kindOverride: s.mediaKind)
        let (query, typedYear) = MetadataLookup.splitYear(id.name)
        let year = s.mediaYear ?? typedYear
        s.lookupGeneration += 1
        let generation = s.lookupGeneration
        s.isLookingUp = true
        s.lookupMessage = nil
        Task {
            var list: [MediaMatch] = []
            var message: String?
            do {
                list = try await MetadataLookup.search(name: query, kind: id.kind, year: year, config: m)
                if list.isEmpty { message = "\(m.provider.label) found nothing for “\(query)”" }
            } catch {
                message = error.localizedDescription
            }
            guard s.lookupGeneration == generation else { return }
            s.lookupCandidates = list
            s.lookupMessage = message
            s.isLookingUp = false
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
        let configured = DiscContent.mode(flags: e.flags, content: { DiscContentProbe.probe(device: e.devicePath) }, drive: cfg)
        guard let chosen = mode ?? configured else {
            lastError = "\(item.displayName): this isn't a DVD or Blu-ray, and the drive is set to leave other discs alone"
            return
        }
        let job = makeJob(for: e, config: cfg, mode: chosen)
        job.usesConfiguredMode = mode == nil && chosen == cfg.rip.mode
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
        job.mediaYear = s.mediaYear
        job.onlineId = s.onlineId.trimmingCharacters(in: .whitespaces)
        if job.discFlags == nil { job.discFlags = s.discFlags }
    }

    func enqueue(_ job: RipJob) {
        jobs.append(job)
        pump()
        saveUnfinishedJobs()
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
        j.mediaYear = job.mediaYear
        j.onlineId = job.onlineId
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
        saveUnfinishedJobs() // also picks up output folders chosen by running jobs
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
            if let w = finished.background { self.background.enqueue(w) }
            if let p = finished.makemkvProblem, p.isLicenseProblem {
                self.makemkvProblem = p
                if p == .keyExpired, self.config.autoUpdateBetaKey, !self.betaKeyTried {
                    self.betaKeyTried = true
                    Task { await self.updateBetaKeyIfNeeded(reason: "after it expired") }
                }
            }
            self.recordHistory(finished)
            self.scheduleRescan(after: 3)
            self.pump()
        }
        job.archivedCandidates = archivedCandidates
        runners[job.id] = runner
        updateKeepAwake()
        Task { await runner.run() }
    }

    /// Whether the Mac should be kept awake: jobs or background steps are running and `preventSleep` is on.
    var wantsAwake: Bool { config.preventSleep && (!runners.isEmpty || background.isBusy || verify.running) }

    /// Keeps the Mac awake (no idle sleep, no App Nap) while jobs or background steps run.
    func updateKeepAwake() {
        let busy = wantsAwake
        guard busy != isKeepingAwake else { return }
        isKeepingAwake = busy
        keepAwakeSwitch(busy)
    }

    private func setProcessActivity(_ on: Bool) {
        if on, keepAwake == nil {
            keepAwake = ProcessInfo.processInfo.beginActivity(options: [.userInitiated, .idleSystemSleepDisabled], reason: "Ripping discs")
        } else if !on, let a = keepAwake {
            ProcessInfo.processInfo.endActivity(a)
            keepAwake = nil
        }
    }

    private func recordHistory(_ job: RipJob) {
        addHistory(HistoryRecord(id: job.id, title: job.title, driveName: job.drive.name, discName: job.discLabel, mode: job.mode,
                                 state: job.state, startedAt: job.startedAt, finishedAt: job.finishedAt,
                                 outputDirectory: job.outputDirectory?.path, files: job.producedFiles.map(\.path),
                                 errorMessage: job.errorMessage, logPath: job.logFile.path,
                                 warnings: job.warningCount, errors: job.errorCount, fingerprint: job.fingerprint))
        saveUnfinishedJobs()
    }

    // MARK: - Archives: already archived, verifying

    /// Verifying archives again (Verify Archive…, the history, the web page, archiveCheck.intervalDays).
    struct VerifyStatus {
        var running = false
        /// Started by archiveCheck.intervalDays.
        var scheduled = false
        /// The last run was stopped.
        var stopped = false
        var path = ""
        var done: Int64 = 0
        var total: Int64 = 0
        var folder: String?
        var file: String?
        /// The folders the last run finished.
        var results: [ArchiveVerifier.FolderCheck] = []
        var finishedAt: Date?
    }

    var verify = VerifyStatus()
    /// When each folder was last verified (archive-checks.json).
    var checkRecords: [String: CheckRecord] = [:]
    @ObservationIgnored private var verifyCancel: CancelFlag?
    @ObservationIgnored private var nextScheduleCheck = Date.distantFuture

    var outputRootURL: URL { URL(fileURLWithPath: Paths.expandTilde(config.outputRoot), isDirectory: true).standardizedFileURL }

    /// The history's successful jobs with their disc fingerprints.
    var archivedCandidates: [ArchivedCandidate] {
        history.compactMap { r in
            guard let fp = r.fingerprint, !fp.isEmpty, let dir = r.outputDirectory else { return nil }
            return ArchivedCandidate(fingerprint: fp, folder: dir, state: r.state, finishedAt: r.finishedAt)
        }
    }

    /// The earlier archive of the disc with this listing according to the history (quick; for the disc page).
    func archivedMatch(for info: DiscInfo?) -> ArchivedMatch? {
        ArchiveLookup.find(fingerprint: DiscFingerprint.of(info), candidates: archivedCandidates, root: nil)
    }

    /// Whether archiveCheck.intervalDays asks for a check of the output root at `now`.
    func verifyDue(at now: Date = Date()) -> Bool {
        let days = config.archiveCheck.intervalDays
        guard days > 0, !verify.running, JobRunner.isDirectory(outputRootURL) else { return false }
        guard let last = checkRecords[outputRootURL.path] else { return true }
        return now.timeIntervalSince(last.checkedAt) >= Double(days) * 86400
    }

    private func startScheduledCheckIfDue() {
        let now = Date()
        guard now >= nextScheduleCheck else { return }
        nextScheduleCheck = now.addingTimeInterval(3600)
        if ownsAutomation && verifyDue(at: now) && activeJobCount == 0 { startVerify(outputRootURL, scheduled: true) }
    }

    /// Verifies every SHA256SUMS folder under `path` in the background. False when a check is already running.
    @discardableResult
    func startVerify(_ path: URL, scheduled: Bool = false) -> Bool {
        guard !verify.running else { return false }
        let flag = CancelFlag()
        verifyCancel = flag
        verify = VerifyStatus(running: true, scheduled: scheduled, path: path.path)
        updateKeepAwake()
        /// Progress written by the worker, read by the main actor.
        final class Live: @unchecked Sendable {
            private let lock = NSLock()
            private var done: Int64 = 0, total: Int64 = 0
            private var folder: String?, file: String?

            func update(_ d: Int64, _ t: Int64, _ f: String?, _ n: String?) {
                lock.lock()
                defer { lock.unlock() }
                done = d
                total = t
                if let f { folder = f }
                if let n { file = n }
            }

            func snapshot() -> (Int64, Int64, String?, String?) {
                lock.lock()
                defer { lock.unlock() }
                return (done, total, folder, file)
            }
        }
        let live = Live()
        let targets = scheduled ? config.notifications : []
        Task {
            let poll = Task { @MainActor [weak self] in
                while !Task.isCancelled {
                    try? await Task.sleep(nanoseconds: 250_000_000)
                    let (d, t, f, n) = live.snapshot()
                    guard let self, self.verify.running else { return }
                    self.verify.done = d
                    self.verify.total = t
                    self.verify.folder = f
                    self.verify.file = n
                }
            }
            let outcome = await Task.detached {
                ArchiveVerifier.verify(folders: ArchiveVerifier.folders(under: path)) { done, total, folder, file in
                    live.update(done, total, folder, file)
                    return !flag.isSet()
                }
            }.value
            poll.cancel()
            if scheduled && !outcome.stopped && !targets.isEmpty {
                let report = Self.verifyReport(outcome.results)
                await NotificationSender.send(targets, title: report.title, body: report.body,
                                              status: report.damaged ? "failed" : "success") { text in NSLog("Bromelia: %@", text) }
            }
            self.finishVerify(path: path, results: outcome.results, stopped: outcome.stopped, scheduled: scheduled)
        }
        return true
    }

    func cancelVerify() { verifyCancel?.set() }

    /// "Archive check: all 12 folder(s) OK" and the damaged folders, for notifications.
    static func verifyReport(_ results: [ArchiveVerifier.FolderCheck]) -> (title: String, body: String, damaged: Bool) {
        let bad = results.filter { !$0.ok }
        let title = bad.isEmpty ? "Archive check: all \(results.count) folder(s) OK" : "Archive check: \(bad.count) of \(results.count) folder(s) damaged"
        var body = bad.prefix(10).map { "\($0.folder): \($0.summary)\n" }.joined()
        if bad.count > 10 { body += "… and \(bad.count - 10) more\n" }
        if bad.isEmpty { body = "Every file matches its checksum." }
        return (title, body, !bad.isEmpty)
    }

    private func finishVerify(path: URL, results: [ArchiveVerifier.FolderCheck], stopped: Bool, scheduled: Bool) {
        let now = Date()
        for r in results { checkRecords[r.folder] = CheckRecord(checkedAt: now, ok: r.ok, summary: r.summary) }
        // The whole tree, so a scheduled check knows when it last ran.
        if !stopped && !results.contains(where: { $0.folder == path.path }) {
            let bad = results.filter { !$0.ok }.count
            checkRecords[path.path] = CheckRecord(checkedAt: now, ok: bad == 0,
                                                  summary: bad > 0 ? "\(bad) of \(results.count) archive folder(s) damaged"
                                                                   : "\(results.count) archive folder(s), all OK")
        }
        if persistent { CheckRecords.save(checkRecords) }
        verify.running = false
        verify.stopped = stopped
        verify.results = results
        verify.finishedAt = now
        verifyCancel = nil
        let report = Self.verifyReport(results)
        if scheduled && report.damaged {
            lastError = report.title
            Notifier.post(title: report.title, body: report.body, sound: true)
        }
        updateKeepAwake()
    }

    // MARK: - Unfinished jobs

    @ObservationIgnored private var savedUnfinished: [UnfinishedJobs.Entry] = []

    /// Writes the queued, waiting and running jobs to unfinished-<pid>.json (only when they changed).
    func saveUnfinishedJobs() {
        guard persistent else { return }
        let entries = jobs.filter { !$0.state.isFinished }.map {
            UnfinishedJobs.Entry(id: $0.id, title: $0.title, driveName: $0.drive.name, discName: $0.discLabel, mode: $0.mode,
                                 state: $0.state, outputDirectory: $0.outputDirectory?.path, logPath: $0.logFile.path, startedAt: $0.startedAt)
        }
        guard entries != savedUnfinished else { return }
        savedUnfinished = entries
        UnfinishedJobs.save(entries)
    }

    /// Records the jobs a Bromelia that has gone left unfinished (see UnfinishedJobs) and says so.
    @discardableResult
    func recoverUnfinishedJobs() -> Int {
        let records = UnfinishedJobs.recover()
        guard !records.isEmpty else { return 0 }
        for r in records { addHistory(r) }
        lastError = "Bromelia stopped last time with \(records.count) unfinished job(s); they are in the history, marked as interrupted or not started"
        return records.count
    }

    private func addHistory(_ rec: HistoryRecord) {
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
    case archiveCheck
}

// MARK: - Web page

extension AppModel {
    private static func webState(_ s: JobState) -> String {
        switch s {
        case .queued: return "queued"
        case .waiting: return "waiting"
        case .running: return "running"
        default: return s.statusWord
        }
    }

    /// The JSON the web page shows (see shared/web/bromelia-web.html).
    func webStatus() -> [String: Any] {
        let iso = ISO8601DateFormatter()
        func time(_ d: Date?) -> String { d.map { iso.string(from: $0) } ?? "" }
        var drives: [[String: Any]] = []
        for item in driveItems {
            guard let e = item.entry else { continue }
            var d: [String: Any] = ["lane": item.laneKey, "name": item.displayName, "device": e.devicePath, "state": e.state.displayName,
                                    "hasDisc": e.state.hasDisc, "disc": e.discName, "busy": activeJob(lane: item.laneKey) != nil,
                                    "config": (item.config ?? config.defaultDrive).id.uuidString]
            if let s = sessions[item.laneKey], s.isLoading || s.info != nil || s.loadError != nil { d["opened"] = Self.webOpened(s) }
            drives.append(d)
        }
        let jobList: [[String: Any]] = jobs.map { j in
            ["id": j.id.uuidString, "title": j.title, "state": Self.webState(j.state), "stateLabel": j.state.label, "phase": j.phase,
             "progress": j.overallProgress, "error": j.errorMessage ?? "", "outputDirectory": j.outputDirectory?.path ?? "", "lane": j.laneKey,
             "mode": j.mode.label, "drive": j.drive.name, "startedAt": time(j.startedAt), "finishedAt": time(j.finishedAt),
             "files": j.producedFiles.count, "warnings": j.warningCount, "errors": j.errorCount]
        }
        let bg: [[String: Any]] = background.items.map { ["id": $0.id.uuidString, "title": $0.work.title, "state": $0.state.rawValue] }
        let hist: [[String: Any]] = history.prefix(20).map {
            ["id": $0.id.uuidString, "title": $0.title, "state": Self.webState($0.state), "stateLabel": $0.state.label, "error": $0.errorMessage ?? "",
             "outputDirectory": $0.outputDirectory ?? "", "finishedAt": time($0.finishedAt), "startedAt": time($0.startedAt),
             "mode": $0.mode.label, "drive": $0.driveName, "files": $0.files.count, "warnings": $0.warnings, "errors": $0.errors]
        }
        let configs = [config.defaultDrive] + config.drives
        let settings: [String: Any] = [
            "drives": configs.map { ["id": $0.id.uuidString, "name": $0.name, "autoRip": $0.automation.autoRipOnInsert, "mode": $0.rip.mode.rawValue,
                                     "isDefault": $0.id == config.defaultDrive.id] },
            "modes": RipMode.videoModes.map { ["value": $0.rawValue, "label": $0.label] }]
        let v = verify
        let damaged: [[String: Any]] = v.results.filter { !$0.ok }.map { ["folder": $0.folder, "summary": $0.summary] }
        let verifyStatus: [String: Any] = [
            "running": v.running, "path": v.path, "progress": v.total > 0 ? Double(v.done) / Double(v.total) : 0, "folder": v.folder ?? "",
            "stopped": v.stopped, "finishedAt": v.finishedAt.map { ISO8601DateFormatter().string(from: $0) } ?? "",
            "folders": v.results.count, "damaged": damaged]
        return ["app": "Bromelia", "version": Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String ?? "",
                "makemkv": makemkvVersion, "problem": makemkvProblem?.explanation ?? "",
                "drives": drives, "jobs": jobList, "background": bg, "history": hist, "verify": verifyStatus, "settings": settings]
    }

    /// An opened disc for the web page: reading, the error, or its titles with the ones chosen on the disc page.
    private static func webOpened(_ s: DiscSession) -> [String: Any] {
        if s.isLoading { return ["loading": true] }
        guard let info = s.info else { return ["loading": false, "error": s.loadError ?? ""] }
        return ["loading": false, "error": "", "name": info.name,
                "titles": info.titles.map { t in
                    ["index": t.index, "name": t.name, "duration": t.durationText, "chapters": t.chapterCount, "size": t.sizeBytes,
                     "selected": s.selectedTitles.contains(t.index)] as [String: Any]
                }]
    }

    /// The end of a job's log (running or in the history), for GET /api/jobs/<id>/log; nil for an unknown job.
    func webLog(id: String) -> String? {
        let path = jobs.first { $0.id.uuidString == id }?.logFile.path ?? history.first { $0.id.uuidString == id }?.logPath
        return path.map { WebLog.tail($0) }
    }

    /// Runs an action from the web page: drives/<lane>/open|rip|eject|close (rip?titles=0,2 rips those titles of the opened
    /// disc), jobs/<id>/cancel, settings/<configuration id>/set?autoRip=1&mode=mkv. Returns an error, or nil.
    func webAction(kind: String, id: String, action: String, query: [String: String] = [:]) -> String? {
        switch (kind, action) {
        case ("drives", "open"):
            guard let item = driveItems.first(where: { $0.laneKey == id }), let e = item.entry, let s = sessions[id] else { return "No such drive" }
            if !e.state.hasDisc { return "There is no disc in the drive" }
            if activeJob(lane: id)?.state == .running { return "The drive is busy" }
            Task { await loadDisc(s) }
            return nil
        case ("drives", "rip"), ("drives", "eject"), ("drives", "close"):
            guard let item = driveItems.first(where: { $0.laneKey == id }), item.entry != nil else { return "No such drive" }
            if action == "rip" {
                if activeJob(lane: id) != nil { return "The drive is busy" }
                let before = jobs.count
                if let list = query["titles"] {
                    guard let s = sessions[id], let info = s.info else { return "Open the disc first" }
                    let chosen = Set(list.split(separator: ",").compactMap { Int($0.trimmingCharacters(in: .whitespaces)) })
                        .intersection(info.titles.map(\.index))
                    if chosen.isEmpty { return "No titles chosen" }
                    s.selectedTitles = chosen
                    ripSession(s, mode: .mkv)
                } else {
                    quickRip(item)
                }
                return jobs.count > before ? nil : (lastError ?? "Nothing to rip")
            }
            if action == "eject" { eject(lane: id) } else { closeTray(lane: id) }
            return nil
        case ("jobs", "cancel"):
            guard let job = jobs.first(where: { $0.id.uuidString == id }) else { return "No such job" }
            cancel(job)
            return nil
        case ("verify", "cancel"):
            cancelVerify()
            return nil
        case ("settings", "set"):
            guard var cfg = config.driveConfig(id: UUID(uuidString: id) ?? UUID()) else { return "No such drive configuration" }
            if let a = query["autoRip"] { cfg.automation.autoRipOnInsert = a == "1" || a == "true" }
            if let m = query["mode"] {
                guard let mode = RipMode(rawValue: m), RipMode.videoModes.contains(mode) else { return "Unknown mode “\(m)”" }
                cfg.rip.mode = mode
            }
            updateDrive(cfg)
            return nil
        case ("verify", "start"):
            if verify.running { return "A check is already running" }
            let root = outputRootURL
            guard JobRunner.isDirectory(root) else { return "The output folder \(root.path) doesn't exist" }
            startVerify(root)
            return nil
        default:
            return "Unknown action"
        }
    }
}
