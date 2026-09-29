import SwiftUI
import AppKit

/// Progress card for a single job (used in drive views and the queue).
struct JobCard: View {
    @Environment(AppModel.self) private var model
    let job: RipJob
    var showLogButton = true
    @State private var showLog = false

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack(alignment: .firstTextBaseline) {
                Image(systemName: job.state.symbol).foregroundStyle(job.state.color)
                Text(job.title).font(.headline).lineLimit(1)
                StatusBadge(text: job.mode.shortLabel, color: .blue)
                if job.isAutomatic { StatusBadge(text: "Automatic", color: .orange) }
                Spacer()
                StatusBadge(text: job.state.label, color: job.state.color)
            }
            if job.state == .running || job.state == .waiting || job.state == .queued {
                TimelineView(.periodic(from: .now, by: 1)) { _ in
                    VStack(alignment: .leading, spacing: 6) {
                        HStack {
                            Text(job.state == .waiting ? "Starting in \(max(0, Int((job.startAt ?? Date()).timeIntervalSinceNow)))s" : job.phase)
                                .font(.callout)
                            Spacer()
                            if job.state == .running {
                                Text(timing).font(.caption).monospacedDigit().foregroundStyle(.secondary)
                            }
                        }
                        if job.state == .running {
                            ProgressView(value: job.overallProgress) {
                                EmptyView()
                            } currentValueLabel: {
                                Text("\(job.totalOperation.isEmpty ? "Overall" : job.totalOperation) — \(Int(job.overallProgress * 100))%")
                                    .font(.caption)
                            }
                            ProgressView(value: min(max(job.currentProgress, 0), 1)) {
                                EmptyView()
                            } currentValueLabel: {
                                Text(job.currentOperation.isEmpty ? " " : job.currentOperation).font(.caption)
                            }
                            .tint(.teal)
                        }
                    }
                }
            } else {
                if let err = job.errorMessage, job.state != .succeeded {
                    Text(err).font(.callout).foregroundStyle(job.state == .completedWithErrors ? .orange : .red).textSelection(.enabled).lineLimit(4)
                }
                HStack(spacing: 12) {
                    Text("\(job.producedFiles.count) item(s) · \(Formatters.duration(job.elapsed))")
                        .font(.caption).foregroundStyle(.secondary)
                    if job.warningCount > 0 { Label("\(job.warningCount)", systemImage: "exclamationmark.triangle").font(.caption).foregroundStyle(.orange) }
                    if job.errorCount > 0 { Label("\(job.errorCount)", systemImage: "xmark.octagon").font(.caption).foregroundStyle(.red) }
                }
            }
            HStack {
                switch job.state {
                case .running:
                    Button("Cancel", role: .destructive) { model.cancel(job) }
                case .waiting:
                    Button("Start Now") { model.startNow(job) }
                    Button("Cancel", role: .destructive) { model.cancel(job) }
                case .queued:
                    Button("Remove", role: .destructive) { model.cancel(job) }
                case .failed, .cancelled, .completedWithErrors:
                    Button("Retry") { model.retry(job) }
                case .succeeded:
                    EmptyView()
                }
                if let dir = job.outputDirectory {
                    Button("Show in Finder") { revealInFinder(dir.path) }
                }
                if showLogButton {
                    Toggle("Log", isOn: $showLog).toggleStyle(.button)
                }
                Spacer()
                if !job.commands.isEmpty {
                    Button {
                        NSPasteboard.general.clearContents()
                        NSPasteboard.general.setString(job.commands.joined(separator: "\n"), forType: .string)
                    } label: { Label("Copy Commands", systemImage: "terminal") }
                    .buttonStyle(.borderless)
                    .help("Copy the makemkvcon command lines used by this job")
                }
            }
            .controlSize(.small)
            if showLog {
                LogView(entries: job.log, autoScroll: job.state == .running)
                    .frame(height: 200)
            }
        }
        .padding(12)
        .background(.background.secondary, in: RoundedRectangle(cornerRadius: 10))
    }

    private var timing: String {
        var s = "Elapsed \(Formatters.duration(job.elapsed))"
        if let r = job.estimatedRemaining { s += " · \(Formatters.duration(r)) left" }
        return s
    }
}

struct QueueView: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        Group {
            if model.jobs.isEmpty && model.background.items.isEmpty {
                ContentUnavailableView {
                    Label("No jobs", systemImage: "tray")
                } description: {
                    Text("Insert a disc and choose Rip, or open a disc to pick titles. Drives set to rip automatically start jobs on their own.")
                }
            } else {
                ScrollView {
                    LazyVStack(spacing: 10) {
                        if !model.background.items.isEmpty {
                            BackgroundList()
                        }
                        ForEach(model.jobs) { job in
                            JobCard(job: job)
                                .contextMenu {
                                    Button("Move Up") { model.moveJob(job, by: -1) }.disabled(job.state != .queued)
                                    Button("Move Down") { model.moveJob(job, by: 1) }.disabled(job.state != .queued)
                                    Divider()
                                    Button("Open Log File") { NSWorkspace.shared.open(job.logFile) }
                                    Button("Show Job Folder") { revealInFinder(job.logDirectory.path) }
                                }
                        }
                    }
                    .padding(16)
                }
            }
        }
        .navigationTitle("Queue")
        .toolbar {
            ToolbarItem {
                Button("Clear Finished") { model.clearFinishedJobs() }
                    .disabled(!model.jobs.contains { $0.state.isFinished })
            }
        }
    }
}

struct HistoryView: View {
    @Environment(AppModel.self) private var model
    @State private var selection: HistoryRecord.ID?
    @State private var confirmClear = false

    var body: some View {
        VStack(spacing: 0) {
            Table(model.history, selection: $selection) {
                TableColumn("Finished") { r in
                    Text(r.finishedAt?.formatted(date: .abbreviated, time: .shortened) ?? "—")
                }
                .width(min: 120, ideal: 150)
                TableColumn("Disc", value: \.discName)
                TableColumn("Drive", value: \.driveName)
                TableColumn("Mode") { r in Text(r.mode.shortLabel) }
                    .width(min: 60, ideal: 90)
                TableColumn("Result") { r in
                    Label(r.state.label, systemImage: r.state.symbol).foregroundStyle(r.state.color)
                }
                .width(min: 80, ideal: 100)
                TableColumn("Items") { r in Text("\(r.files.count)").monospacedDigit() }
                    .width(40)
                TableColumn("Duration") { r in
                    if let s = r.startedAt, let f = r.finishedAt { Text(Formatters.duration(f.timeIntervalSince(s))).monospacedDigit() }
                }
                .width(70)
                TableColumn("Verified") { r in
                    if let dir = r.outputDirectory, let c = model.checkRecords[dir] {
                        Label(c.ok ? "OK" : "Damaged", systemImage: c.ok ? "checkmark.shield" : "exclamationmark.triangle.fill")
                            .foregroundStyle(c.ok ? Color.secondary : Color.red)
                            .help("\(c.checkedAt.formatted(date: .abbreviated, time: .shortened)): \(c.summary)")
                    }
                }
                .width(min: 60, ideal: 90)
            }
            .contextMenu(forSelectionType: HistoryRecord.ID.self) { ids in
                if let id = ids.first, let r = model.history.first(where: { $0.id == id }) {
                    if let dir = r.outputDirectory { Button("Show Output in Finder") { revealInFinder(dir) } }
                    Button("Open Log") { NSWorkspace.shared.open(URL(fileURLWithPath: r.logPath)) }
                }
            } primaryAction: { ids in
                if let id = ids.first, let r = model.history.first(where: { $0.id == id }), let dir = r.outputDirectory {
                    revealInFinder(dir)
                }
            }
            if let id = selection, let r = model.history.first(where: { $0.id == id }) {
                Divider()
                HistoryDetail(record: r)
                    .frame(height: 220)
            }
        }
        .navigationTitle("History")
        .toolbar {
            ToolbarItem {
                Button("Clear History…") { confirmClear = true }
                    .disabled(model.history.isEmpty)
            }
        }
        .confirmationDialog("Clear the job history and delete the stored job logs?", isPresented: $confirmClear) {
            Button("Clear History", role: .destructive) { model.clearHistory() }
        }
    }
}

struct HistoryDetail: View {
    @Environment(AppModel.self) private var model
    let record: HistoryRecord
    @State private var logText = ""

    var body: some View {
        HSplitView {
            VStack(alignment: .leading, spacing: 6) {
                Text(record.title).font(.headline)
                if let e = record.errorMessage { Text(e).foregroundStyle(.red).textSelection(.enabled) }
                if let d = record.outputDirectory {
                    Button(d) { revealInFinder(d) }.buttonStyle(.link)
                    if FileManager.default.fileExists(atPath: URL(fileURLWithPath: d).appendingPathComponent(Checksums.fileName).path) {
                        HStack {
                            Button("Verify Folder") {
                                if !model.startVerify(URL(fileURLWithPath: d, isDirectory: true)) {
                                    model.lastError = "An archive check is already running"
                                }
                                model.selection = .archiveCheck
                            }
                            .help("Read the files again and compare them with SHA256SUMS")
                            if let c = model.checkRecords[d] {
                                Text("Last verified \(c.checkedAt.formatted(date: .abbreviated, time: .shortened)): \(c.summary)")
                                    .font(.caption).foregroundStyle(c.ok ? Color.secondary : Color.red)
                            } else {
                                Text("Not verified since it was archived").font(.caption).foregroundStyle(.secondary)
                            }
                        }
                    }
                }
                List(record.files, id: \.self) { f in
                    Text((f as NSString).lastPathComponent).help(f)
                }
                .listStyle(.plain)
            }
            .padding(10)
            .frame(minWidth: 260)
            ScrollView {
                Text(logText)
                    .font(.system(.caption, design: .monospaced))
                    .textSelection(.enabled)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding(8)
            }
        }
        .task(id: record.id) {
            logText = (try? String(contentsOfFile: record.logPath, encoding: .utf8)) ?? "Log not available"
        }
    }
}

/// Reads archive folders again and compares every file with its SHA256SUMS (bit rot, bad copies).
struct ArchiveCheckView: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        let v = model.verify
        let root = model.outputRootURL
        VStack(alignment: .leading, spacing: 10) {
            HStack {
                Text("Archive Check").font(.title2.bold())
                Spacer()
                if v.running { Button("Stop") { model.cancelVerify() } }
                Button("Check a Folder…") { checkFolder() }
                    .disabled(v.running)
                Button("Check Output Folder") { checkOutputFolder() }
                    .buttonStyle(.borderedProminent)
                    .disabled(v.running)
            }
            Text("Reads every file listed in each archive folder's SHA256SUMS again and compares it with its checksum, to find files damaged on the disk (bit rot) or by a bad copy. Files missing from the folder and files SHA256SUMS doesn't list are reported too.")
                .font(.callout).foregroundStyle(.secondary)
            Group {
                if let c = model.checkRecords[root.path] {
                    Text("Output folder \(root.path): last checked \(c.checkedAt.formatted(date: .abbreviated, time: .standard)) — \(c.summary)")
                } else {
                    Text("Output folder \(root.path): never checked")
                }
                if model.config.archiveCheck.intervalDays > 0 {
                    Text("Checked every \(model.config.archiveCheck.intervalDays) day(s) (Settings).")
                }
            }
            .font(.caption).foregroundStyle(.secondary)
            if v.running {
                ProgressView(value: v.total > 0 ? Double(v.done) / Double(v.total) : 0) {
                    Text("\(v.folder ?? v.path)\(v.file.map { " — " + $0 } ?? "")").lineLimit(1).truncationMode(.middle)
                } currentValueLabel: {
                    Text("\(ByteCountFormatter.string(fromByteCount: v.done, countStyle: .file)) of \(ByteCountFormatter.string(fromByteCount: v.total, countStyle: .file))")
                }
            } else if let when = v.finishedAt {
                let damaged = v.results.filter { !$0.ok }.count
                Text(v.results.isEmpty ? "\(when.formatted(date: .abbreviated, time: .standard)): no archive folders (with a SHA256SUMS) found in \(v.path)"
                     : "\(when.formatted(date: .abbreviated, time: .standard))\(v.stopped ? " (stopped)" : ""): \(v.results.count) folder(s) checked, \(damaged > 0 ? "some are damaged" : "all OK")")
                    .font(.headline)
            }
            ScrollView {
                LazyVStack(alignment: .leading, spacing: 8) {
                    ForEach(v.results, id: \.folder) { r in
                        ArchiveFolderRow(result: r)
                        Divider()
                    }
                }
                .padding(.trailing, 8)
            }
            .opacity(v.running ? 0.5 : 1)
        }
        .padding()
        .navigationTitle("Archive Check")
    }

    private func checkFolder() {
        let panel = NSOpenPanel()
        panel.title = "Verify an Archive Folder"
        panel.canChooseFiles = false
        panel.canChooseDirectories = true
        panel.directoryURL = model.outputRootURL
        if panel.runModal() == .OK, let url = panel.url { model.startVerify(url) }
    }

    private func checkOutputFolder() {
        let root = model.outputRootURL
        guard JobRunner.isDirectory(root) else {
            model.lastError = "The output folder \(root.path) doesn't exist"
            return
        }
        model.startVerify(root)
    }
}

private struct ArchiveFolderRow: View {
    let result: ArchiveVerifier.FolderCheck
    @State private var expanded: Bool

    init(result: ArchiveVerifier.FolderCheck) {
        self.result = result
        _expanded = State(initialValue: !result.ok)
    }

    var body: some View {
        DisclosureGroup(isExpanded: $expanded) {
            ForEach(Array(zip(["changed", "unreadable", "missing", "not listed in SHA256SUMS"],
                              [result.changed, result.unreadable, result.missing, result.extra])), id: \.0) { what, files in
                ForEach(files.prefix(50), id: \.self) { f in
                    HStack {
                        Text(f).textSelection(.enabled)
                        Spacer()
                        Text(what).foregroundStyle(what == "not listed in SHA256SUMS" ? Color.secondary : Color.red)
                    }
                    .font(.callout)
                }
                if files.count > 50 { Text("… and \(files.count - 50) more \(what)").font(.caption).foregroundStyle(.secondary) }
            }
        } label: {
            HStack {
                Image(systemName: result.ok ? "checkmark.shield" : "exclamationmark.triangle.fill")
                    .foregroundStyle(result.ok ? Color.green : Color.red)
                VStack(alignment: .leading) {
                    Text((result.folder as NSString).lastPathComponent)
                    Text(result.folder).font(.caption).foregroundStyle(.secondary).lineLimit(1).truncationMode(.middle)
                    Text(result.summary).font(.caption).foregroundStyle(result.ok ? Color.secondary : Color.red)
                }
                Spacer()
                Button { revealInFinder(result.folder) } label: { Image(systemName: "folder") }
                    .buttonStyle(.borderless).help("Show in Finder")
            }
        }
    }
}

/// Colour-coded log list with a severity filter.
struct LogView: View {
    let entries: [LogEntry]
    var autoScroll: Bool
    @State private var hideInfo = false

    var body: some View {
        let shown = hideInfo ? entries.filter { $0.severity == .warning || $0.severity == .error } : entries
        VStack(spacing: 0) {
            ScrollViewReader { proxy in
                List(shown) { e in
                    HStack(alignment: .firstTextBaseline, spacing: 8) {
                        Text(e.time, format: .dateTime.hour().minute().second())
                            .foregroundStyle(.tertiary)
                        Text(e.text)
                            .foregroundStyle(e.severity.color)
                            .textSelection(.enabled)
                    }
                    .font(.system(.caption, design: .monospaced))
                    .listRowInsets(EdgeInsets(top: 1, leading: 6, bottom: 1, trailing: 6))
                    .id(e.id)
                }
                .listStyle(.plain)
                .onChange(of: entries.last?.id) { _, last in
                    if autoScroll, let last { proxy.scrollTo(last, anchor: .bottom) }
                }
            }
            HStack {
                Toggle("Warnings and errors only", isOn: $hideInfo).toggleStyle(.checkbox)
                Spacer()
                Button("Copy") {
                    let text = entries.map { "\(RipJob.logTimeFormatter.string(from: $0.time)) \($0.text)" }.joined(separator: "\n")
                    NSPasteboard.general.clearContents()
                    NSPasteboard.general.setString(text, forType: .string)
                }
            }
            .controlSize(.small)
            .padding(.horizontal, 8)
            .padding(.vertical, 4)
        }
        .background(Color(nsColor: .textBackgroundColor))
        .clipShape(RoundedRectangle(cornerRadius: 6))
    }
}

/// Background post-processing (encoding, uploads) of finished jobs.
struct BackgroundList: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack {
                Text("Background").font(.headline)
                Spacer()
                Button("Clear Finished") { model.background.clearFinished() }.buttonStyle(.borderless)
            }
            ForEach(model.background.items) { item in
                HStack {
                    Image(systemName: item.state == .done ? "checkmark.circle" : item.state == .failed ? "xmark.octagon" : item.state == .running ? "gearshape.2" : "clock")
                        .foregroundStyle(item.state == .failed ? .red : item.state == .done ? .green : .secondary)
                    Text(item.work.title).lineLimit(1)
                    Spacer()
                    Text(item.message.isEmpty ? item.state.rawValue.capitalized : item.message).font(.caption).foregroundStyle(.secondary).lineLimit(1)
                }
            }
        }
        .padding(12)
        .background(.quaternary.opacity(0.4), in: RoundedRectangle(cornerRadius: 10))
    }
}
