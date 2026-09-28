import SwiftUI
import UniformTypeIdentifiers

struct ContentView: View {
    @Environment(AppModel.self) private var model
    @State private var isDropTargeted = false

    var body: some View {
        @Bindable var model = model
        NavigationSplitView {
            Sidebar()
                .navigationSplitViewColumnWidth(min: 230, ideal: 260, max: 360)
        } detail: {
            detail
                .frame(maxWidth: .infinity, maxHeight: .infinity)
        }
        .toolbar {
            ToolbarItemGroup(placement: .primaryAction) {
                Button {
                    openSourcePanel(model)
                } label: { Label("Open Image", systemImage: "doc.badge.plus") }
                .help("Open an ISO image, a BDMV / VIDEO_TS folder, or a file inside one (.IFO, .mpls, .m2ts, …)")
                Button {
                    Task { await model.refreshDrives(force: true) }
                } label: { Label("Rescan", systemImage: "arrow.clockwise") }
                .disabled(model.isScanning)
                .help("Rescan optical drives")
                SettingsLink { Label("Settings", systemImage: "gearshape") }
            }
        }
        .onDrop(of: [.fileURL], isTargeted: $isDropTargeted) { providers in
            for p in providers {
                _ = p.loadObject(ofClass: URL.self) { url, _ in
                    if let url { DispatchQueue.main.async { model.openFileSource(url) } }
                }
            }
            return true
        }
        .overlay(alignment: .top) {
            VStack(spacing: 8) {
                if let p = model.makemkvProblem { MakeMKVProblemBanner(problem: p) }
                if let err = model.lastError {
                    ErrorBanner(text: err) { model.lastError = nil }
                }
            }
            .padding(.top, 8)
        }
    }

    @ViewBuilder
    private var detail: some View {
        switch model.selection {
        case .drive(let id)?:
            if let item = model.driveItem(id: id) {
                DriveDetailView(item: item)
                    .id(item.id)
            } else {
                ContentUnavailableView("Drive not available", systemImage: "opticaldiscdrive",
                                       description: Text("The drive was disconnected."))
            }
        case .source(let key)?:
            if let s = model.sessions[key] {
                SourceDetailView(session: s)
                    .id(key)
            } else {
                ContentUnavailableView("Nothing selected", systemImage: "opticaldisc")
            }
        case .history?:
            HistoryView()
        case .queue?, nil:
            QueueView()
        }
    }
}

struct ErrorBanner: View {
    let text: String
    let dismiss: () -> Void

    var body: some View {
        HStack(alignment: .top) {
            Image(systemName: "exclamationmark.triangle.fill").foregroundStyle(.yellow)
            Text(text).textSelection(.enabled)
            Spacer()
            Button(action: dismiss) { Image(systemName: "xmark") }.buttonStyle(.borderless)
        }
        .padding(10)
        .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 8))
        .shadow(radius: 4)
        .frame(maxWidth: 560)
    }
}

/// Expired key, outdated MakeMKV or evaluation not started, with the way out.
struct MakeMKVProblemBanner: View {
    @Environment(AppModel.self) private var model
    let problem: MakeMKVNotice
    @State private var working = false
    @State private var result: String?

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack(alignment: .top) {
                Image(systemName: "key.fill").foregroundStyle(.orange)
                Text(problem.explanation).textSelection(.enabled)
                Spacer()
                Button { model.makemkvProblem = nil } label: { Image(systemName: "xmark") }.buttonStyle(.borderless)
            }
            HStack {
                Button(working ? "Getting the Beta Key…" : "Get the Current Beta Key") {
                    Task {
                        working = true
                        result = await model.installBetaKey()
                        working = false
                    }
                }
                .disabled(working)
                if problem == .versionTooOld {
                    Button("Download MakeMKV") { NSWorkspace.shared.open(URL(string: "https://www.makemkv.com/download/")!) }
                }
                SettingsLink { Text("Enter a Key…") }
            }
            if let result { Text(result).font(.caption).textSelection(.enabled) }
        }
        .padding(10)
        .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 8))
        .shadow(radius: 4)
        .frame(maxWidth: 560)
    }
}

struct Sidebar: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        @Bindable var model = model
        List(selection: $model.selection) {
            Section {
                ForEach(model.driveItems) { item in
                    DriveSidebarRow(item: item)
                        .tag(SidebarSelection.drive(item.id))
                        .contextMenu { DriveContextMenu(item: item) }
                }
                if model.driveItems.isEmpty {
                    Text(model.isScanning ? "Scanning…" : "No optical drives found")
                        .foregroundStyle(.secondary)
                        .font(.callout)
                }
            } header: {
                HStack {
                    Text("Drives")
                    Spacer()
                    if model.isScanning { ProgressView().controlSize(.mini) }
                }
            }

            if !model.fileSessionIds.isEmpty {
                Section("Images & Folders") {
                    ForEach(model.fileSessionIds, id: \.self) { key in
                        if let s = model.sessions[key] {
                            SourceSidebarRow(session: s)
                                .tag(SidebarSelection.source(key))
                                .contextMenu {
                                    Button("Close") { model.closeFileSource(key) }
                                }
                        }
                    }
                }
            }

            Section("Jobs") {
                Label {
                    HStack {
                        Text("Queue")
                        Spacer()
                        let active = model.jobs.filter { !$0.state.isFinished }.count
                        if active > 0 { Text("\(active)").monospacedDigit().foregroundStyle(.secondary) }
                    }
                } icon: { Image(systemName: "list.bullet.rectangle") }
                .tag(SidebarSelection.queue)
                Label("History", systemImage: "clock.arrow.circlepath")
                    .tag(SidebarSelection.history)
            }
        }
        .listStyle(.sidebar)
        .safeAreaInset(edge: .bottom) {
            VStack(alignment: .leading, spacing: 2) {
                if !model.makemkvVersion.isEmpty {
                    Text(model.makemkvVersion).font(.caption2).foregroundStyle(.secondary)
                }
                if let t = model.lastScan {
                    Text("Scanned \(t.formatted(date: .omitted, time: .standard))").font(.caption2).foregroundStyle(.tertiary)
                }
            }
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding(.horizontal, 12)
            .padding(.bottom, 8)
        }
    }
}

struct DriveSidebarRow: View {
    @Environment(AppModel.self) private var model
    let item: DriveItem

    var body: some View {
        let job = model.activeJob(lane: item.laneKey)
        HStack(spacing: 8) {
            ZStack {
                Image(systemName: item.entry?.state == .inserted ? "opticaldisc.fill" : "opticaldiscdrive")
                    .foregroundStyle(item.isConnected ? Color.accentColor : .secondary)
                if let job, job.state == .running {
                    ProgressView(value: job.overallProgress)
                        .progressViewStyle(.circular)
                        .controlSize(.small)
                        .opacity(0.9)
                        .offset(x: 9, y: 7)
                        .scaleEffect(0.6)
                }
            }
            .frame(width: 22)
            VStack(alignment: .leading, spacing: 1) {
                HStack(spacing: 4) {
                    Text(item.displayName).lineLimit(1)
                    if item.config == nil && item.isConnected {
                        Image(systemName: "exclamationmark.circle").foregroundStyle(.orange).help("Not configured — using the default configuration")
                    }
                    if item.config?.automation.autoRipOnInsert == true {
                        Image(systemName: "bolt.fill").foregroundStyle(.yellow).font(.caption2).help("Automatic rip on insert")
                    }
                }
                Text(subtitle(job)).font(.caption).foregroundStyle(.secondary).lineLimit(1)
            }
        }
        .padding(.vertical, 2)
    }

    private func subtitle(_ job: RipJob?) -> String {
        if let job {
            switch job.state {
            case .running: return "\(job.phase) · \(Int(job.overallProgress * 100))%"
            case .waiting:
                let s = max(0, Int((job.startAt ?? Date()).timeIntervalSinceNow))
                return "Auto-rip in \(s)s"
            case .queued: return "Queued"
            default: break
            }
        }
        guard let e = item.entry else { return "Disconnected" }
        if e.state == .inserted { return e.discName.isEmpty ? e.flags.discTypeName : "\(e.discName) · \(e.flags.discTypeName)" }
        return e.state.displayName
    }
}

struct DriveContextMenu: View {
    @Environment(AppModel.self) private var model
    let item: DriveItem

    var body: some View {
        if let e = item.entry {
            Button("Open Disc") {
                model.selection = .drive(item.id)
                if let s = model.session(for: item) { Task { await model.loadDisc(s) } }
            }
            .disabled(e.state != .inserted)
            Menu("Rip Using Drive Rules") {
                ForEach(RipMode.allCases) { mode in
                    Button(mode.label) { model.quickRip(item, mode: mode) }
                }
            }
            .disabled(e.state != .inserted)
            Button("Eject") { model.eject(lane: item.laneKey) }
            Divider()
            if item.config == nil {
                Button("Set Up This Drive…") {
                    model.configure(e)
                    model.selection = .drive(model.config.driveConfig(for: e)!.id.uuidString)
                }
            }
        } else if let c = item.config {
            Button("Remove Configuration") { model.removeDriveConfig(c.id) }
        }
    }
}

struct SourceSidebarRow: View {
    let session: DiscSession

    var body: some View {
        HStack(spacing: 8) {
            Image(systemName: {
                if case .iso = session.source { return "opticaldisc" }
                return "folder"
            }())
            .frame(width: 22)
            VStack(alignment: .leading, spacing: 1) {
                Text(session.source.displayName).lineLimit(1)
                Text(session.isLoading ? "Reading…" : (session.info?.name ?? session.loadError ?? ""))
                    .font(.caption).foregroundStyle(.secondary).lineLimit(1)
            }
        }
    }
}
