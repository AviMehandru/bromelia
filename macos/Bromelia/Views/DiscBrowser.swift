import SwiftUI
import AppKit

enum DiscNode: Hashable {
    case disc
    case title(Int)
    case track(Int, Int)
}

/// MakeMKV-style title tree with checkboxes, an information panel and rip actions.
struct DiscBrowser: View {
    @Environment(AppModel.self) private var model
    let session: DiscSession
    let info: DiscInfo
    let config: DriveConfig
    let driveBusy: Bool
    @State private var selectedNode: DiscNode? = .disc
    @State private var expanded: Set<Int> = []
    @State private var showLog = false

    var body: some View {
        let decisions = Dictionary(uniqueKeysWithValues: TitleSelector.evaluate(info.titles, rule: config.rip.titleSelection).decisions.map { ($0.titleIndex, $0) })
        let longest = info.titles.max { $0.durationSeconds < $1.durationSeconds }?.index
        VStack(spacing: 0) {
            HSplitView {
                List(selection: $selectedNode) {
                    Label {
                        VStack(alignment: .leading) {
                            Text(info.name.isEmpty ? "Disc" : info.name).font(.headline)
                            Text("\(info.typeName) · \(info.titles.count) title(s)").font(.caption).foregroundStyle(.secondary)
                        }
                    } icon: { Image(systemName: "opticaldisc") }
                    .tag(DiscNode.disc)

                    ForEach(info.titles) { title in
                        TitleRow(session: session, title: title, decision: decisions[title.index], isLongest: title.index == longest,
                                 expanded: Binding(get: { expanded.contains(title.index) },
                                                   set: { if $0 { expanded.insert(title.index) } else { expanded.remove(title.index) } }),
                                 hasMkvmerge: model.mkvmerge != nil)
                            .tag(DiscNode.title(title.index))
                        if expanded.contains(title.index) {
                            ForEach(title.tracks) { track in
                                TrackRow(session: session, title: title, track: track)
                                    .tag(DiscNode.track(title.index, track.index))
                                    .padding(.leading, 34)
                            }
                        }
                    }
                }
                .listStyle(.inset(alternatesRowBackgrounds: true))
                .frame(minWidth: 420)

                InfoPanel(session: session, info: info, node: selectedNode ?? .disc)
                    .frame(minWidth: 260, idealWidth: 320)
            }
            Divider()
            identityBar
            Divider()
            actionBar
            if showLog {
                Divider()
                LogView(entries: session.log, autoScroll: false).frame(height: 180)
            }
        }
    }

    private var actionBar: some View {
        let selected = session.selectedTitles.count
        let customTracks = session.trackSelections.keys.filter { session.selectedTitles.contains($0) }.count
        return HStack(spacing: 10) {
            Menu("Select") {
                Button("All Titles") { session.selectedTitles = Set(info.titles.map(\.index)) }
                Button("None") { session.selectedTitles = [] }
                Button("Apply “\(config.name)” Title Rules") { session.applyRule(config.rip.titleSelection) }
                Divider()
                Button("Expand All") { expanded = Set(info.titles.map(\.index)) }
                Button("Collapse All") { expanded = [] }
            }
            .fixedSize()
            VStack(alignment: .leading, spacing: 1) {
                Text("\(selected) of \(info.titles.count) titles · \(ByteCountFormatter.string(fromByteCount: session.selectedSizeBytes, countStyle: .file))")
                    .font(.callout)
                Text(outputPreview).font(.caption).foregroundStyle(.secondary).lineLimit(1).truncationMode(.middle)
                if let ld = session.libreDrive {
                    Text(ld.label).font(.caption).foregroundStyle(ld == .required ? .red : .secondary).lineLimit(1)
                        .help("Blu-ray and 4K UHD discs read through LibreDrive need a supported drive and firmware; MakeMKV reports it when opening the disc.")
                }
            }
            Button {
                chooseOutputFolder()
            } label: {
                Image(systemName: session.outputFolderOverride.isEmpty ? "folder" : "folder.fill.badge.gearshape")
            }
            .buttonStyle(.borderless)
            .help(session.outputFolderOverride.isEmpty ? "Choose a different output folder for this disc" : "Output folder: \(session.outputFolderOverride)")
            .contextMenu {
                Button("Use the Configuration's Folder") { session.outputFolderOverride = "" }
                    .disabled(session.outputFolderOverride.isEmpty)
            }
            if customTracks > 0 {
                StatusBadge(text: "Custom tracks in \(customTracks) title(s)", color: model.mkvmerge == nil ? .red : .purple)
                    .help(model.mkvmerge == nil ? "mkvmerge is required for custom track selection" : "Unselected tracks are removed with mkvmerge after ripping")
            }
            Spacer()
            Toggle(isOn: $showLog) { Image(systemName: "text.alignleft") }
                .toggleStyle(.button)
                .help("Show the disc log")
            if case .drive = session.source {
                Menu {
                    Button(RipMode.backup.label) { model.ripSession(session, mode: .backup) }
                    Button(RipMode.backupDecrypted.label) { model.ripSession(session, mode: .backupDecrypted) }
                    Button("Decrypted backup, then MKV of selected titles") { model.ripSession(session, mode: .backupThenMkv) }
                } label: {
                    Label("Backup", systemImage: "externaldrive")
                }
                .fixedSize()
                .disabled(driveBusy)
            }
            Button {
                model.ripSession(session, mode: .mkv)
            } label: {
                Label("Make MKV", systemImage: "film.stack")
            }
            .keyboardShortcut(.return, modifiers: [.command])
            .buttonStyle(.borderedProminent)
            .disabled(selected == 0 || driveBusy)
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 8)
    }

    /// What the disc is, as used for file names: editable name, movie / TV and first episode number.
    private var identity: MediaIdentity {
        MediaIdentity.resolve(info: info, discLabel: info.name, flags: session.discFlags, encrypted: false,
                              nameOverride: session.mediaName, kindOverride: session.mediaKind)
    }

    private var identityBar: some View {
        @Bindable var session = session
        let auto = MediaIdentity.resolve(info: info, discLabel: info.name, flags: session.discFlags, encrypted: false)
        let id = identity
        return HStack(spacing: 10) {
            Image(systemName: id.kind == .tv ? "tv" : "film")
                .foregroundStyle(.secondary)
            TextField("Name", text: $session.mediaName, prompt: Text(auto.name))
                .frame(minWidth: 160, maxWidth: 260)
                .help("Movie or show name used for folder and file names. Empty = “\(auto.name)”, read from the disc.")
            Picker("", selection: $session.mediaKind) {
                Text("Auto (\(auto.kind.label))").tag(MediaKind?.none)
                ForEach(MediaKind.allCases) { Text($0.label).tag(MediaKind?.some($0)) }
            }
            .labelsHidden()
            .fixedSize()
            .help(session.mediaKind == nil ? "Detected: \(auto.reason)" : "Chosen for this disc")
            if id.kind == .tv {
                TextField("First episode", value: $session.firstEpisode, format: .number, prompt: Text("First ep."))
                    .frame(width: 70)
                    .help("Number of the first episode on this disc. Empty = read from the disc menus, or 1.")
            }
            StatusBadge(text: id.formatCode, color: id.format == .uhd ? .orange : id.format == .bluray ? .blue : .secondary)
                .help(id.format.label)
            Text(sampleFileName(id))
                .font(.caption).foregroundStyle(.secondary).lineLimit(1).truncationMode(.middle)
                .textSelection(.enabled)
            Spacer(minLength: 0)
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 6)
    }

    private func sampleFileName(_ id: MediaIdentity) -> String {
        let template = config.output.fileNameTemplate.trimmingCharacters(in: .whitespaces)
        guard !template.isEmpty else { return "MakeMKV's file names" }
        let title = info.titles.first { session.selectedTitles.contains($0.index) } ?? info.titles.first
        var v = TemplateRenderer.dateValues()
        for (k, x) in id.templateValues(rip: "Rip") { v[k] = x }
        v["disc"] = info.name; v["volume"] = info.volumeName; v["type"] = info.typeToken; v["drive"] = config.name; v["job"] = "xxxxxxxx"
        if let title {
            for (k, x) in JobRunner.titleValues(title, ordinal: 1, disc: info, originalFile: nil) { v[k] = x }
            v["track"] = MediaIdentity.trackLabel(title)
        }
        if id.kind == .tv {
            v["episode"] = MediaIdentity.episodeLabel(session.firstEpisode ?? 1, width: 2)
            v["episodeNumber"] = String(session.firstEpisode ?? 1)
        }
        return "e.g. " + TemplateRenderer.renderPath(template, values: v) + ".mkv"
    }

    private func chooseOutputFolder() {
        let panel = NSOpenPanel()
        panel.canChooseFiles = false
        panel.canChooseDirectories = true
        panel.canCreateDirectories = true
        panel.prompt = "Use Folder"
        panel.message = "Files from this disc will be saved directly into the chosen folder."
        if panel.runModal() == .OK, let url = panel.url { session.outputFolderOverride = url.path }
    }

    private var outputPreview: String {
        if !session.outputFolderOverride.isEmpty { return "→ " + session.outputFolderOverride + "  (one-off folder, right-click the folder button to reset)" }
        var v = TemplateRenderer.dateValues()
        v["disc"] = info.name.isEmpty ? "Disc" : info.name
        v["volume"] = info.volumeName
        v["type"] = info.typeToken
        v["drive"] = config.name
        v["job"] = "xxxxxxxx"
        for (k, x) in identity.templateValues(rip: "Rip") { v[k] = x }
        let root = Paths.expandTilde(model.config.outputRoot(for: config))
        let rel = TemplateRenderer.renderPath(config.output.folderTemplate, values: v)
        return "→ " + (rel.isEmpty ? root : (root as NSString).appendingPathComponent(rel))
    }
}

struct TitleRow: View {
    let session: DiscSession
    let title: TitleInfo
    let decision: TitleDecision?
    let isLongest: Bool
    @Binding var expanded: Bool
    let hasMkvmerge: Bool

    var body: some View {
        HStack(spacing: 6) {
            Button { expanded.toggle() } label: {
                Image(systemName: "chevron.right")
                    .rotationEffect(.degrees(expanded ? 90 : 0))
                    .frame(width: 12)
            }
            .buttonStyle(.borderless)
            Toggle("", isOn: Binding(
                get: { session.selectedTitles.contains(title.index) },
                set: { if $0 { session.selectedTitles.insert(title.index) } else { session.selectedTitles.remove(title.index) } }
            ))
            .toggleStyle(.checkbox)
            .labelsHidden()
            Image(systemName: "film")
                .foregroundStyle(.secondary)
            VStack(alignment: .leading, spacing: 1) {
                HStack(spacing: 6) {
                    Text("Title \(title.index)").fontWeight(.medium)
                    Text(title.durationText).monospacedDigit()
                    Text("\(title.chapterCount) ch").foregroundStyle(.secondary)
                    Text(title.sizeText).foregroundStyle(.secondary)
                    if isLongest { StatusBadge(text: "Longest", color: .green) }
                    if let a = title.angle, a > 0 { StatusBadge(text: "Angle \(a)", color: .blue) }
                    if session.hasCustomTracks(title.index) { StatusBadge(text: "Custom tracks", color: .purple) }
                }
                Text(subtitle).font(.caption).foregroundStyle(.secondary).lineLimit(1)
            }
            Spacer()
            if let d = decision {
                Image(systemName: d.selected ? "checkmark.seal" : "minus.circle")
                    .foregroundStyle(d.selected ? .green : .secondary)
                    .help("Drive rules: \(d.reason)")
            }
        }
        .contextMenu {
            if session.hasCustomTracks(title.index) {
                Button("Use the Profile's Track Selection") { session.resetTracks(title.index) }
            } else {
                Button("Choose Tracks Manually") { session.customizeTracks(title.index); expanded = true }
                    .disabled(!hasMkvmerge)
            }
        }
    }

    private var subtitle: String {
        var parts: [String] = []
        if !title.name.isEmpty { parts.append(title.name) }
        if let src = title.sourceTitleId { parts.append("source #\(src)") }
        if !title.sourceFileName.isEmpty { parts.append(title.sourceFileName) }
        if !title.outputFileName.isEmpty { parts.append("→ \(title.outputFileName)") }
        if !title.segmentMap.isEmpty { parts.append("segments \(title.segmentMap)") }
        return parts.joined(separator: " · ")
    }
}

struct TrackRow: View {
    let session: DiscSession
    let title: TitleInfo
    let track: TrackInfo

    var body: some View {
        HStack(spacing: 6) {
            if session.hasCustomTracks(title.index) {
                Toggle("", isOn: Binding(
                    get: { session.isTrackSelected(title: title.index, track: track.index) },
                    set: { session.setTrack(title: title.index, track: track.index, selected: $0) }
                ))
                .toggleStyle(.checkbox)
                .labelsHidden()
            } else {
                Image(systemName: "circle.dotted").foregroundStyle(.tertiary).frame(width: 14)
                    .help("Chosen by the profile's selection rule. Right-click the title to choose tracks manually.")
            }
            Image(systemName: icon).foregroundStyle(.secondary).frame(width: 16)
            Text(track.summary.isEmpty ? track.kind.rawValue.capitalized : track.summary).lineLimit(1)
            if !track.languageName.isEmpty { Text(track.languageName).foregroundStyle(.secondary) }
            ForEach(track.flags.descriptions, id: \.self) { StatusBadge(text: $0, color: .gray) }
            if track.isDefault { StatusBadge(text: "Default", color: .blue) }
            Spacer()
            if let conv = track.attribute(.outputConversionType), !conv.isEmpty {
                Text(conv).font(.caption).foregroundStyle(.tertiary)
            }
        }
    }

    private var icon: String {
        switch track.kind {
        case .video: return "video"
        case .audio: return "speaker.wave.2"
        case .subtitle: return "captions.bubble"
        case .attachment: return "paperclip"
        case .other: return "questionmark.square.dashed"
        }
    }
}

/// Attribute table for the selected disc / title / track, like MakeMKV's info pane.
struct InfoPanel: View {
    let session: DiscSession
    let info: DiscInfo
    let node: DiscNode

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 10) {
                Text(heading).font(.headline)
                if case .title(let t) = node, let title = info.title(at: t) {
                    VStack(alignment: .leading, spacing: 3) {
                        Text("Output file name").font(.caption).foregroundStyle(.secondary)
                        TextField("Output file name", text: Binding(
                            get: { session.titleNameOverrides[t] ?? "" },
                            set: { session.titleNameOverrides[t] = $0.isEmpty ? nil : $0 }
                        ), prompt: Text(title.outputFileName.isEmpty ? "MakeMKV default" : title.outputFileName))
                        .labelsHidden()
                        Text("Overrides the configuration's file name template. {tokens} are allowed.")
                            .font(.caption2).foregroundStyle(.tertiary)
                    }
                }
                Grid(alignment: .leadingFirstTextBaseline, horizontalSpacing: 10, verticalSpacing: 5) {
                    ForEach(rows, id: \.0) { row in
                        GridRow {
                            Text(row.0).foregroundStyle(.secondary).gridColumnAlignment(.trailing)
                            Text(row.1).textSelection(.enabled)
                        }
                    }
                }
                .font(.callout)
            }
            .padding(14)
            .frame(maxWidth: .infinity, alignment: .leading)
        }
        .background(Color(nsColor: .controlBackgroundColor))
    }

    private var heading: String {
        switch node {
        case .disc: return "Disc information"
        case .title(let t): return "Title \(t)"
        case .track(let t, let s): return "Title \(t) · Track \(s)"
        }
    }

    private var rows: [(String, String)] {
        let attrs: [Int: String]
        switch node {
        case .disc: attrs = info.attributes
        case .title(let t): attrs = info.title(at: t)?.attributes ?? [:]
        case .track(let t, let s): attrs = info.title(at: t)?.tracks.first { $0.index == s }?.attributes ?? [:]
        }
        var out: [(String, String)] = []
        for key in attrs.keys.sorted() {
            guard let v = attrs[key], !v.isEmpty else { continue }
            let id = AttributeID(rawValue: key)
            if let id, !id.isUserVisible { continue }
            var value = v
            if id == .streamFlags, let n = Int(v) {
                let d = StreamFlags(rawValue: n).descriptions
                value = d.isEmpty ? v : d.joined(separator: ", ")
            }
            out.append((id?.displayName ?? "Attribute \(key)", value))
        }
        return out
    }
}
