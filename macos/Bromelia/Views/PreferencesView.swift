import SwiftUI
import UniformTypeIdentifiers
import AppKit

struct PreferencesView: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        @Bindable var model = model
        TabView {
            GeneralPreferences()
                .tabItem { Label("General", systemImage: "gearshape") }
            MakeMKVSettingsTab(settings: $model.config.globalSettings, mode: .global)
                .tabItem { Label("MakeMKV", systemImage: "slider.horizontal.3") }
            DriveConfigEditor(config: $model.config.defaultDrive, isDefaultTemplate: true)
                .tabItem { Label("Default Drive", systemImage: "opticaldiscdrive") }
            DrivesPreferences()
                .tabItem { Label("Drives & Presets", systemImage: "square.stack.3d.up") }
            PostProcessTab(steps: $model.config.plugins, driveName: "All drives",
                           emptyText: "Plugins are post-processing steps for every drive, usually limited to a movie or show (by name or disc label) and to formats such as DVD or 4Ke — for example a script that archives one series in a particular way. They run after the drive's own steps.")
                .padding(12)
                .tabItem { Label("Plugins", systemImage: "puzzlepiece.extension") }
            RegistrationPreferences()
                .tabItem { Label("Registration", systemImage: "key") }
        }
        .frame(width: 860, height: 640)
    }
}

private struct GeneralPreferences: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        @Bindable var model = model
        Form {
            Section("Tools") {
                PathField(title: "makemkvcon", path: $model.config.makemkvconPath, kind: .file,
                          placeholder: model.makemkvcon?.path ?? "Not found — install MakeMKV")
                statusLine(model.makemkvcon, missing: "makemkvcon not found. Install MakeMKV from makemkv.com.")
                PathField(title: "mkvmerge", path: $model.config.mkvmergePath, kind: .file,
                          placeholder: model.mkvmerge?.path ?? "Not found — optional")
                statusLine(model.mkvmerge, missing: "mkvmerge not found. Install MKVToolNix to choose individual tracks.")
            }
            Section("Output") {
                PathField(title: "Default output folder", path: $model.config.outputRoot, kind: .directory)
            }
            Section("Drive detection") {
                LabeledContent("Poll drives every") {
                    IntField(title: "Seconds", value: $model.config.pollIntervalSeconds, suffix: "seconds (0 = only on media events)")
                }
                Toggle("Keep polling while jobs are running", isOn: $model.config.pollWhileRipping)
                Text("Disc insertion and removal are also detected instantly through macOS. Polling runs makemkvcon to refresh drive and disc names; pausing it during rips avoids disturbing busy drives.")
                    .font(.caption).foregroundStyle(.secondary)
            }
            Section("Jobs") {
                LabeledContent("Maximum simultaneous jobs") {
                    IntField(title: "Jobs", value: $model.config.maxConcurrentJobs, suffix: "(0 = one per drive, no global limit)")
                }
                LabeledContent("Keep history for") {
                    IntField(title: "Jobs", value: $model.config.historyLimit, suffix: "jobs")
                }
            }
            Section("MakeMKV") {
                HStack {
                    Button("Import Settings from MakeMKV") {
                        model.importSettings(MakeMKVEnvironment.installedSettings())
                    }
                    Button("Open MakeMKV Settings Folder") {
                        NSWorkspace.shared.open(Paths.makemkvUserFolder)
                    }
                    Button("Open Bromelia Data Folder") {
                        try? Paths.ensureDirectory(Paths.appSupport)
                        NSWorkspace.shared.open(Paths.appSupport)
                    }
                }
                if !model.makemkvVersion.isEmpty {
                    LabeledContent("Detected version", value: model.makemkvVersion)
                }
                ForEach(Array(model.scanMessages.enumerated()), id: \.offset) { _, m in
                    Label(m.text, systemImage: m.severity == .error ? "xmark.octagon" : "info.circle")
                        .foregroundStyle(m.severity.color)
                        .font(.caption)
                }
            }
        }
        .formStyle(.grouped)
    }

    @ViewBuilder
    private func statusLine(_ url: URL?, missing: String) -> some View {
        if let url {
            Label(url.path, systemImage: "checkmark.circle.fill").font(.caption).foregroundStyle(.green)
        } else {
            Label(missing, systemImage: "exclamationmark.triangle.fill").font(.caption).foregroundStyle(.orange)
        }
    }
}

private struct DrivesPreferences: View {
    @Environment(AppModel.self) private var model
    @State private var editing: DriveConfig?
    @State private var message: String?

    var body: some View {
        @Bindable var model = model
        Form {
            Section("Drive configurations") {
                if model.config.drives.isEmpty {
                    Text("No drives configured yet. Select a drive in the main window and choose “Set Up This Drive…”.")
                        .foregroundStyle(.secondary)
                }
                ForEach(model.config.drives) { d in
                    HStack {
                        VStack(alignment: .leading) {
                            Text(d.name)
                            Text(d.match.driveName.isEmpty ? d.match.devicePath : d.match.driveName)
                                .font(.caption).foregroundStyle(.secondary)
                        }
                        Spacer()
                        ConfigSummary(config: d).lineLimit(1)
                        Button("Edit…") { editing = d }
                        Button { duplicate(d) } label: { Image(systemName: "plus.square.on.square") }
                            .buttonStyle(.borderless).help("Duplicate")
                    }
                }
            }
            Section("Presets") {
                if model.config.presets.isEmpty {
                    Text("Save a drive configuration as a preset from its editor to reuse it on other drives.")
                        .foregroundStyle(.secondary)
                }
                ForEach($model.config.presets) { $p in
                    HStack {
                        TextField("Name", text: $p.name)
                        Spacer()
                        Button(role: .destructive) {
                            model.config.presets.removeAll { $0.id == p.id }
                        } label: { Image(systemName: "trash") }
                        .buttonStyle(.borderless)
                    }
                }
            }
            Section("Import / export") {
                HStack {
                    Button("Export Drives and Presets…") { exportAll() }
                    Button("Import…") { importAll() }
                }
                Text("Exports are plain JSON and can be used by the Windows and Linux versions of Bromelia.")
                    .font(.caption).foregroundStyle(.secondary)
                if let message { Text(message).font(.caption) }
            }
        }
        .formStyle(.grouped)
        .sheet(item: $editing) { cfg in
            DriveConfigSheet(original: cfg, session: nil)
        }
    }

    private func duplicate(_ d: DriveConfig) {
        var c = d
        c.id = UUID()
        c.name = d.name + " copy"
        c.match = DriveMatch()
        c.postProcess = c.postProcess.map { var s = $0; s.id = UUID(); return s }
        model.config.drives.append(c)
    }

    private func exportAll() {
        let panel = NSSavePanel()
        panel.nameFieldStringValue = "bromelia-drives.json"
        panel.allowedContentTypes = [.json]
        guard panel.runModal() == .OK, let url = panel.url else { return }
        let bundle = ConfigStore.ExportBundle(drives: model.config.drives, presets: model.config.presets)
        do {
            try ConfigStore.encoder().encode(bundle).write(to: url)
            message = "Exported \(bundle.drives.count) drive(s) and \(bundle.presets.count) preset(s)."
        } catch { message = error.localizedDescription }
    }

    private func importAll() {
        let panel = NSOpenPanel()
        panel.allowedContentTypes = [.json]
        guard panel.runModal() == .OK, let url = panel.url else { return }
        do {
            let data = try Data(contentsOf: url)
            let bundle = try ConfigStore.decoder().decode(ConfigStore.ExportBundle.self, from: data)
            var added = 0
            for d in bundle.drives {
                if let i = model.config.drives.firstIndex(where: { $0.id == d.id }) { model.config.drives[i] = d } else { model.config.drives.append(d); added += 1 }
            }
            for p in bundle.presets where !model.config.presets.contains(where: { $0.id == p.id }) { model.config.presets.append(p) }
            message = "Imported \(bundle.drives.count) drive(s) (\(added) new) and \(bundle.presets.count) preset(s)."
        } catch { message = "Import failed: \(error.localizedDescription)" }
    }
}

private struct RegistrationPreferences: View {
    @Environment(AppModel.self) private var model
    @State private var reveal = false
    @State private var result: String?
    @State private var working = false

    var body: some View {
        @Bindable var model = model
        Form {
            Section {
                HStack {
                    if reveal {
                        TextField("Registration key", text: $model.config.registrationKey)
                            .font(.system(.body, design: .monospaced))
                    } else {
                        SecureField("Registration key", text: $model.config.registrationKey)
                    }
                    Button { reveal.toggle() } label: { Image(systemName: reveal ? "eye.slash" : "eye") }
                        .buttonStyle(.borderless)
                }
                let installed = MakeMKVEnvironment.installedRegistrationKey() != nil
                Text(model.config.registrationKey.isEmpty
                     ? (installed ? "Empty: the key MakeMKV is registered with is used." : "No key found. MakeMKV runs in evaluation / beta mode.")
                     : "This key is passed to makemkvcon for every drive.")
                    .font(.caption).foregroundStyle(.secondary)
            } header: {
                Text("MakeMKV registration")
            }
            Section {
                HStack {
                    Button(working ? "Registering…" : "Register Key with MakeMKV") {
                        Task {
                            working = true
                            result = await model.registerWithMakeMKV(key: model.config.registrationKey)
                            working = false
                        }
                    }
                    .disabled(model.config.registrationKey.isEmpty || working)
                    Button("Get a Key…") { NSWorkspace.shared.open(URL(string: "https://www.makemkv.com/buy/")!) }
                    Button("Beta Key…") { NSWorkspace.shared.open(URL(string: "https://forum.makemkv.com/forum/viewtopic.php?f=5&t=1053")!) }
                }
                if let result { Text(result).font(.caption).textSelection(.enabled) }
                Text("“Register Key with MakeMKV” runs makemkvcon reg, which stores the key in MakeMKV's own settings so the MakeMKV app uses it too.")
                    .font(.caption).foregroundStyle(.secondary)
            }
        }
        .formStyle(.grouped)
    }
}

/// Wrapper around MakeMKV's universal firmware tool (read-only commands).
struct DriveToolsView: View {
    @Environment(AppModel.self) private var model
    @State private var drive = ""
    @State private var output = ""
    @State private var running = false

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text("Drive tools use MakeMKV's firmware utility (makemkvcon f). Only read-only commands are offered here; flashing firmware must be done from the command line.")
                .font(.callout).foregroundStyle(.secondary)
            HStack {
                Button("List Drives") { run(["f", "-l"]) }
                TextField("Drive (device or name from the list)", text: $drive)
                Menu("Use Drive") {
                    ForEach(model.scannedDrives.filter(\.isPresent), id: \.self) { e in
                        Button("\(e.driveName) (\(e.devicePath))") { drive = e.devicePath }
                    }
                }
                .fixedSize()
                Button("Drive Commands") { run(["f", "-d", drive, "help"]) }.disabled(drive.isEmpty)
                Button("SDF Info") { run(["f", "--info"]) }
            }
            .disabled(running)
            ScrollView {
                Text(output.isEmpty ? " " : output)
                    .font(.system(.caption, design: .monospaced))
                    .textSelection(.enabled)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding(8)
            }
            .background(Color(nsColor: .textBackgroundColor), in: RoundedRectangle(cornerRadius: 6))
        }
        .padding()
    }

    private func run(_ args: [String]) {
        guard let exe = model.makemkvcon else { output = "makemkvcon not found"; return }
        running = true
        output = "$ makemkvcon " + args.map(ArgumentSplitter.quote).joined(separator: " ") + "\n"
        Task {
            let collector = LineCollector()
            let runner = ProcessRunner(executable: exe, arguments: args)
            let out = try? await runner.run(timeout: 120) { collector.append($0) }
            output += collector.all.joined(separator: "\n") + "\n\n(exit status \(out?.exitCode ?? -1))"
            running = false
        }
    }
}

/// Compact status window shown from the menu bar.
struct MenuBarContent: View {
    @Environment(AppModel.self) private var model
    @Environment(\.openWindow) private var openWindow

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text("Bromelia").font(.headline)
            ForEach(model.driveItems.filter(\.isConnected)) { item in
                let job = model.activeJob(lane: item.laneKey)
                VStack(alignment: .leading, spacing: 3) {
                    HStack {
                        Image(systemName: item.entry?.state == .inserted ? "opticaldisc.fill" : "opticaldiscdrive")
                        Text(item.displayName).fontWeight(.medium)
                        Spacer()
                        if item.entry?.state == .inserted && job == nil {
                            Button("Rip") { model.quickRip(item) }.controlSize(.small)
                        }
                        Button { model.eject(lane: item.laneKey) } label: { Image(systemName: "eject") }
                            .buttonStyle(.borderless)
                    }
                    if let job {
                        Text(job.phase).font(.caption).foregroundStyle(.secondary)
                        ProgressView(value: job.overallProgress)
                    } else if let e = item.entry {
                        Text(e.state == .inserted ? (e.discName.isEmpty ? e.flags.discTypeName : e.discName) : e.state.displayName)
                            .font(.caption).foregroundStyle(.secondary)
                    }
                }
            }
            if model.driveItems.filter(\.isConnected).isEmpty {
                Text("No drives").foregroundStyle(.secondary)
            }
            Divider()
            HStack {
                Button("Open Bromelia") {
                    openWindow(id: "main")
                    NSApp.activate(ignoringOtherApps: true)
                }
                Spacer()
                Button("Quit") { NSApp.terminate(nil) }
            }
        }
        .padding(12)
        .frame(width: 300)
    }
}
