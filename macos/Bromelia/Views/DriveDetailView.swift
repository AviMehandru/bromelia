import SwiftUI

struct DriveDetailView: View {
    @Environment(AppModel.self) private var model
    let item: DriveItem
    @State private var editing: DriveConfig?

    var body: some View {
        let session = model.session(for: item)
        let job = model.activeJob(lane: item.laneKey)
        VStack(spacing: 0) {
            header(job: job, session: session)
                .padding(16)
            Divider()
            if let job {
                JobCard(job: job)
                    .padding(12)
                Divider()
            }
            if let session {
                DiscArea(session: session, config: item.config ?? model.config.defaultDrive, driveBusy: job?.state == .running)
            } else {
                ContentUnavailableView("Drive not connected", systemImage: "opticaldiscdrive",
                                       description: Text("This configuration will be used as soon as a drive matching “\(item.config?.match.driveName ?? "")” is connected."))
            }
        }
        .frame(maxHeight: .infinity, alignment: .top)
        .navigationTitle(item.displayName)
        .sheet(item: $editing) { cfg in
            DriveConfigSheet(original: cfg, session: session)
        }
    }

    @ViewBuilder
    private func header(job: RipJob?, session: DiscSession?) -> some View {
        HStack(alignment: .top, spacing: 14) {
            Image(systemName: item.entry?.state == .inserted ? "opticaldisc.fill" : "opticaldiscdrive.fill")
                .font(.system(size: 38))
                .foregroundStyle(Color.accentColor.gradient)
                .frame(width: 48)
            VStack(alignment: .leading, spacing: 4) {
                HStack {
                    Text(item.displayName).font(.title2.weight(.semibold))
                    if item.config == nil { StatusBadge(text: "Default configuration", color: .orange) }
                    if item.config?.enabled == false { StatusBadge(text: "Disabled", color: .gray) }
                }
                if let e = item.entry {
                    Text(e.driveName).font(.callout).foregroundStyle(.secondary).textSelection(.enabled)
                    HStack(spacing: 10) {
                        Label(e.devicePath.isEmpty ? "disc:\(e.index)" : e.devicePath, systemImage: "cable.connector")
                        Label(e.state.displayName, systemImage: "circle.fill")
                            .foregroundStyle(e.state == .inserted ? .green : .secondary)
                        if e.state == .inserted {
                            Label(e.discName.isEmpty ? e.flags.discTypeName : "\(e.discName) (\(e.flags.discTypeName))", systemImage: "opticaldisc")
                        }
                    }
                    .font(.caption)
                    .labelStyle(.titleAndIcon)
                }
                ConfigSummary(config: item.config ?? model.config.defaultDrive)
                    .padding(.top, 2)
            }
            Spacer()
            VStack(alignment: .trailing, spacing: 8) {
                HStack {
                    if let e = item.entry {
                        Button {
                            if let session { Task { await model.loadDisc(session) } }
                        } label: { Label("Open Disc", systemImage: "list.bullet.indent") }
                        .disabled(e.state != .inserted || session?.isLoading == true || job?.state == .running)

                        Menu {
                            ForEach(RipMode.allCases) { mode in
                                Button(mode.label) { model.quickRip(item, mode: mode) }
                            }
                        } label: {
                            Label("Rip", systemImage: "record.circle")
                        } primaryAction: {
                            model.quickRip(item)
                        }
                        .help("Rip with this drive's configured mode and title rules")
                        .fixedSize()
                        .disabled(e.state != .inserted || job != nil)

                        Button { model.eject(lane: item.laneKey) } label: { Label("Eject", systemImage: "eject") }
                            .disabled(job?.state == .running)
                    }
                }
                HStack {
                    if let e = item.entry, item.config == nil {
                        Button("Set Up This Drive…") {
                            let c = model.configure(e)
                            editing = c
                        }
                    } else if let c = item.config {
                        Button("Configure…") { editing = c }
                    }
                }
            }
        }
    }
}

/// Compact chips describing a drive configuration.
struct ConfigSummary: View {
    let config: DriveConfig

    var body: some View {
        HStack(spacing: 6) {
            StatusBadge(text: config.rip.mode.shortLabel, color: .blue)
            StatusBadge(text: titleRuleText, color: .purple)
            StatusBadge(text: config.profile.mode == .makemkvDefault ? "Default profile" : (config.profile.mode == .generated ? "Profile: \(config.profile.generated.name)" : "Custom profile"), color: .teal)
            if !config.settings.isEmpty { StatusBadge(text: "\(config.settings.count) setting override(s)", color: .indigo) }
            let steps = config.postProcess.filter(\.enabled).count
            if steps > 0 { StatusBadge(text: "\(steps) post-process step(s)", color: .pink) }
            if config.automation.autoRipOnInsert { StatusBadge(text: "Auto-rip", color: .orange) }
        }
    }

    private var titleRuleText: String {
        let r = config.rip.titleSelection
        switch r.strategy {
        case .all: return "All titles"
        case .longest: return r.longestCount == 1 ? "Main feature" : "Longest \(r.longestCount)"
        case .indices: return "Titles \(r.indexPattern)"
        case .manual: return "Manual titles"
        }
    }
}

struct SourceDetailView: View {
    @Environment(AppModel.self) private var model
    let session: DiscSession

    var body: some View {
        @Bindable var session = session
        let job = model.activeJob(lane: session.id)
        VStack(spacing: 0) {
            HStack(alignment: .center, spacing: 14) {
                Image(systemName: { if case .iso = session.source { return "opticaldisc.fill" }; return "folder.fill" }())
                    .font(.system(size: 34))
                    .foregroundStyle(Color.accentColor.gradient)
                VStack(alignment: .leading, spacing: 3) {
                    Text(session.info?.name ?? session.source.displayName).font(.title2.weight(.semibold))
                    Text(session.source.infoArgument).font(.caption).foregroundStyle(.secondary).textSelection(.enabled)
                }
                Spacer()
                Picker("Configuration", selection: $session.configId) {
                    Text(model.config.defaultDrive.name).tag(model.config.defaultDrive.id)
                    ForEach(model.config.drives) { d in Text(d.name).tag(d.id) }
                }
                .frame(maxWidth: 260)
                .onChange(of: session.configId) { _, _ in
                    session.applyRule(model.configForSession(session).rip.titleSelection)
                }
                Button { Task { await model.loadDisc(session) } } label: { Label("Reload", systemImage: "arrow.clockwise") }
                    .disabled(session.isLoading)
                Button { model.closeFileSource(session.id) } label: { Label("Close", systemImage: "xmark") }
            }
            .padding(16)
            Divider()
            if let job {
                JobCard(job: job).padding(12)
                Divider()
            }
            DiscArea(session: session, config: model.configForSession(session), driveBusy: false)
        }
        .frame(maxHeight: .infinity, alignment: .top)
        .navigationTitle(session.source.displayName)
    }
}

/// Loading state, errors, or the title browser for a disc session.
struct DiscArea: View {
    @Environment(AppModel.self) private var model
    let session: DiscSession
    let config: DriveConfig
    let driveBusy: Bool

    var body: some View {
        if session.isLoading {
            VStack(spacing: 12) {
                ProgressView(value: session.progress) {
                    Text(session.operation.isEmpty ? "Reading disc…" : session.operation)
                }
                .frame(maxWidth: 420)
                Button("Cancel") { session.runner?.cancel() }
                LogView(entries: session.log, autoScroll: true)
                    .frame(maxHeight: 260)
            }
            .padding()
            .frame(maxHeight: .infinity)
        } else if let info = session.info {
            DiscBrowser(session: session, info: info, config: config, driveBusy: driveBusy)
        } else if let err = session.loadError {
            VStack(spacing: 12) {
                ContentUnavailableView("Could not open the disc", systemImage: "exclamationmark.triangle", description: Text(err))
                    .frame(maxHeight: 220)
                Button("Try Again") { Task { await model.loadDisc(session) } }
                if !session.log.isEmpty {
                    LogView(entries: session.log, autoScroll: false).frame(maxHeight: 240).padding(.horizontal)
                }
            }
            .frame(maxHeight: .infinity)
        } else {
            ContentUnavailableView {
                Label("No disc opened", systemImage: "opticaldisc")
            } description: {
                Text("Open the disc to browse titles and tracks, or use Rip to apply this drive's rules directly.")
            } actions: {
                if case .drive = session.source {
                    Button("Open Disc") { Task { await model.loadDisc(session) } }
                        .disabled(driveBusy)
                }
            }
        }
    }
}
