#if DEBUG
import SwiftUI
import AppKit

/// Development aid: renders the main screens with sample data to PNG files and quits.
///
///     Bromelia.app/Contents/MacOS/Bromelia -BromeliaSnapshotDir /tmp/shots -BromeliaSnapshotFixture shared/fixtures/info-dvd.txt
enum DebugSnapshots {
    static var requested: Bool { UserDefaults.standard.string(forKey: "BromeliaSnapshotDir") != nil }

    @MainActor
    static func run() {
        guard let dir = UserDefaults.standard.string(forKey: "BromeliaSnapshotDir") else { return }
        let out = URL(fileURLWithPath: dir, isDirectory: true)
        try? FileManager.default.createDirectory(at: out, withIntermediateDirectories: true)

        let model = AppModel(persistent: false)
        let fixturePath = UserDefaults.standard.string(forKey: "BromeliaSnapshotFixture") ?? ""
        let text = (try? String(contentsOfFile: fixturePath, encoding: .utf8)) ?? ""
        let info = DiscInfoBuilder.build(fromOutput: text)

        let drives = [
            DriveScanEntry(index: 0, state: .inserted, flags: [.dvdFiles], driveName: "BD-RE HL-DT-ST BD-RE  WH16NS60 1.02 KLAM6E84325", discName: info.name, devicePath: "/dev/rdisk4"),
            DriveScanEntry(index: 1, state: .inserted, flags: [.blurayFiles, .aacsFiles], driveName: "BD-RE ASUS BW-16D1HT 3.10 KL3H4AB1234", discName: "MOVIE_DISC", devicePath: "/dev/rdisk5"),
            DriveScanEntry(index: 2, state: .emptyClosed, flags: [], driveName: "DVD+R-DL MATSHITA DVD-R   UJ8C2 SB00", discName: "", devicePath: "/dev/rdisk6"),
        ]
        var left = model.config.defaultDrive
        left.id = UUID(); left.name = "Left drive"; left.match = DriveMatch(driveName: drives[0].driveName)
        left.rip.titleSelection.strategy = .longest
        left.profile.mode = .generated
        var step = PostProcessStep(); step.name = "Move to library"; step.executable = "/bin/mv"
        step.matchName = "^One Piece$"; step.matchFormats = ["DVD", "DVDe"]
        left.postProcess = [step]
        var plugin = PostProcessStep(); plugin.name = "Archive 4K discs to NAS"; plugin.executable = "~/bin/archive-uhd.sh"
        plugin.matchFormats = ["4K", "4Ke"]
        model.config.plugins = [plugin]
        var right = model.config.defaultDrive
        right.id = UUID(); right.name = "Right drive"; right.match = DriveMatch(driveName: drives[1].driveName)
        right.rip.mode = .backupThenMkv
        right.automation.autoRipOnInsert = true
        model.config.drives = [left, right]
        model.debugApplyScan(drives)

        let leftLane = DriveItem.laneKey(for: drives[0])
        if let s = model.sessions[leftLane] {
            s.info = info
            s.applyRule(left.rip.titleSelection)
            s.customizeTracks(1)
            s.setTrack(title: 1, track: 4, selected: false)
        }
        let job = model.makeJob(for: drives[1], config: right, mode: .backupThenMkv)
        job.state = .running
        job.startedAt = Date().addingTimeInterval(-754)
        job.phase = "Backing up disc (decrypted)"
        job.totalOperation = "Saving to file"
        job.currentOperation = "Copying BDMV/STREAM/00800.m2ts"
        job.stepCount = 2
        job.totalProgress = 0.62
        job.currentProgress = 0.35
        job.discLabel = "MOVIE_DISC"
        model.jobs = [job]
        let done = model.makeJob(for: drives[0], config: left, mode: .mkv)
        done.state = .succeeded
        done.startedAt = Date().addingTimeInterval(-3600)
        done.finishedAt = Date().addingTimeInterval(-3100)
        done.discLabel = info.name
        done.producedFiles = [URL(fileURLWithPath: "/Movies/x.mkv")]
        done.outputDirectory = URL(fileURLWithPath: "/Movies")
        model.jobs.append(done)

        model.selection = .drive(left.id.uuidString)
        render(ContentView().environment(model), size: CGSize(width: 1280, height: 800), to: out.appendingPathComponent("main-drive.png"))
        model.selection = .queue
        render(ContentView().environment(model), size: CGSize(width: 1280, height: 800), to: out.appendingPathComponent("main-queue.png"))
        model.selection = .drive(right.id.uuidString)
        render(ContentView().environment(model), size: CGSize(width: 1280, height: 800), to: out.appendingPathComponent("main-drive-busy.png"))
        render(DriveConfigSheet(original: left, session: model.sessions[leftLane]).environment(model),
               size: CGSize(width: 900, height: 720), to: out.appendingPathComponent("config-sheet.png"))
        render(PreferencesView().environment(model), size: CGSize(width: 860, height: 640), to: out.appendingPathComponent("preferences.png"))
        for (name, view) in configTabs(left, session: model.sessions[leftLane]) {
            render(view.environment(model), size: CGSize(width: 880, height: 720), to: out.appendingPathComponent("tab-\(name).png"))
        }
        NSApp.terminate(nil)
    }

    @MainActor
    private static func configTabs(_ c: DriveConfig, session: DiscSession?) -> [(String, AnyView)] {
        [
            ("rip", AnyView(ConfigTabHost(config: c) { b in RipTab(config: b, previewInfo: session?.info) })),
            ("output", AnyView(ConfigTabHost(config: c) { b in OutputTab(config: b) })),
            ("settings", AnyView(ConfigTabHost(config: c) { b in MakeMKVSettingsTab(settings: b.settings, mode: .drive) })),
            ("profile", AnyView(ConfigTabHost(config: c) { b in ProfileTab(profile: b.profile) })),
            ("post", AnyView(ConfigTabHost(config: c) { b in PostProcessTab(steps: b.postProcess, driveName: c.name) })),
        ]
    }

    @MainActor
    private static func render<V: View>(_ view: V, size: CGSize, to url: URL) {
        let hosting = NSHostingView(rootView: view
            .frame(width: size.width, height: size.height)
            .background(Color(nsColor: .windowBackgroundColor)))
        hosting.frame = CGRect(origin: .zero, size: size)
        let window = NSWindow(contentRect: hosting.frame, styleMask: [.titled], backing: .buffered, defer: false)
        let dark = UserDefaults.standard.bool(forKey: "BromeliaSnapshotDark")
        window.appearance = NSAppearance(named: dark ? .darkAqua : .aqua)
        hosting.appearance = window.appearance
        window.contentView = hosting
        window.layoutIfNeeded()
        RunLoop.current.run(until: Date().addingTimeInterval(0.8))
        hosting.layoutSubtreeIfNeeded()
        guard let rep = hosting.bitmapImageRepForCachingDisplay(in: hosting.bounds) else { return }
        hosting.cacheDisplay(in: hosting.bounds, to: rep)
        try? rep.representation(using: .png, properties: [:])?.write(to: url)
        window.close()
    }
}

/// Holds a DriveConfig in @State so editor tabs can be rendered in isolation.
private struct ConfigTabHost<Content: View>: View {
    @State var config: DriveConfig
    let content: (Binding<DriveConfig>) -> Content

    init(config: DriveConfig, @ViewBuilder content: @escaping (Binding<DriveConfig>) -> Content) {
        _config = State(initialValue: config)
        self.content = content
    }

    var body: some View { content($config) }
}
#endif
