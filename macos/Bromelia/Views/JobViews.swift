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
    let record: HistoryRecord
    @State private var logText = ""

    var body: some View {
        HSplitView {
            VStack(alignment: .leading, spacing: 6) {
                Text(record.title).font(.headline)
                if let e = record.errorMessage { Text(e).foregroundStyle(.red).textSelection(.enabled) }
                if let d = record.outputDirectory {
                    Button(d) { revealInFinder(d) }.buttonStyle(.link)
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
