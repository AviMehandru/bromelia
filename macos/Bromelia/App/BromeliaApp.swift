import SwiftUI
import AppKit
import UniformTypeIdentifiers

struct BromeliaApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) private var appDelegate
    @State private var model = AppModel()

    var body: some Scene {
        Window("Bromelia", id: "main") {
            ContentView()
                .environment(model)
                .frame(minWidth: 980, minHeight: 620)
                .onAppear {
                    appDelegate.model = model
                    // Unit tests use the app as host; don't touch drives or settings there.
                    if ProcessInfo.processInfo.environment["XCTestConfigurationFilePath"] == nil && !AppDelegate.isSnapshotRun {
                        model.start()
                    }
                }
        }
        .commands { BromeliaCommands(model: model) }

        Settings {
            PreferencesView()
                .environment(model)
        }

        Window("Drive Tools", id: "drive-tools") {
            DriveToolsView()
                .environment(model)
        }
        .defaultSize(width: 720, height: 480)

        MenuBarExtra {
            MenuBarContent()
                .environment(model)
        } label: {
            Image(systemName: model.activeJobCount > 0 ? "opticaldisc.fill" : "opticaldisc")
        }
        .menuBarExtraStyle(.window)
    }
}

final class AppDelegate: NSObject, NSApplicationDelegate {
    weak var model: AppModel?

    static var isSnapshotRun: Bool {
        #if DEBUG
        return DebugSnapshots.requested
        #else
        return false
        #endif
    }

    func applicationDidFinishLaunching(_ notification: Notification) {
        #if DEBUG
        if DebugSnapshots.requested {
            DispatchQueue.main.async { MainActor.assumeIsolated { DebugSnapshots.run() } }
        }
        #endif
    }

    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        MainActor.assumeIsolated {
            guard let model, model.activeJobCount > 0 else {
                model?.saveNow()
                return .terminateNow
            }
            let alert = NSAlert()
            alert.messageText = "\(model.activeJobCount) job(s) are still running"
            alert.informativeText = "Quitting will cancel them. Partially written files are left in place."
            alert.addButton(withTitle: "Keep Running")
            alert.addButton(withTitle: "Cancel Jobs and Quit")
            alert.alertStyle = .warning
            if alert.runModal() == .alertSecondButtonReturn {
                for job in model.jobs where job.state == .running { model.cancel(job) }
                model.saveNow()
                return .terminateNow
            }
            return .terminateCancel
        }
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { false }
}

struct BromeliaCommands: Commands {
    let model: AppModel
    @Environment(\.openWindow) private var openWindow

    var body: some Commands {
        CommandGroup(replacing: .newItem) {
            Button("Open Disc Image or Folder…") { openSourcePanel(model) }
                .keyboardShortcut("o")
        }
        CommandMenu("Drives") {
            Button("Rescan Drives") { Task { await model.refreshDrives(force: true) } }
                .keyboardShortcut("r")
            Divider()
            Button("Drive Tools…") { openWindow(id: "drive-tools") }
        }
        CommandMenu("Jobs") {
            Button("Show Queue") { model.selection = .queue }
                .keyboardShortcut("1", modifiers: [.command, .option])
            Button("Show History") { model.selection = .history }
                .keyboardShortcut("2", modifiers: [.command, .option])
            Button("Verify Archive…") { model.selection = .archiveCheck }
            Divider()
            Button("Clear Finished Jobs") { model.clearFinishedJobs() }
        }
        CommandGroup(replacing: .help) {
            Button("MakeMKV Documentation") { NSWorkspace.shared.open(URL(string: "https://www.makemkv.com/developers/")!) }
            Button("MakeMKV Forum") { NSWorkspace.shared.open(URL(string: "https://forum.makemkv.com/")!) }
        }
    }
}

@MainActor
func openSourcePanel(_ model: AppModel) {
    let panel = NSOpenPanel()
    panel.title = "Open Disc Image, Disc Folder or a File on a Disc"
    panel.message = "Choose an ISO image, or a folder containing BDMV / VIDEO_TS"
    panel.canChooseFiles = true
    panel.canChooseDirectories = true
    panel.allowsMultipleSelection = true
    panel.treatsFilePackagesAsDirectories = true
    if panel.runModal() == .OK {
        for url in panel.urls { model.openFileSource(url) }
    }
}
