import SwiftUI

struct PostProcessTab: View {
    @Binding var steps: [PostProcessStep]
    let driveName: String
    var emptyText = "Post-processing steps run after each job, in order. Use them to move, rename, encode or catalogue files, or to call any script."
    @State private var selected: PostProcessStep.ID?

    var body: some View {
        HSplitView {
            VStack(spacing: 0) {
                List(selection: $selected) {
                    ForEach(steps) { step in
                        HStack {
                            Image(systemName: step.enabled ? "terminal.fill" : "terminal")
                                .foregroundStyle(step.enabled ? Color.accentColor : .secondary)
                            VStack(alignment: .leading) {
                                Text(step.name)
                                Text(step.runOn.label + (step.perFile ? " · per file" : "") + Self.conditionSummary(step))
                                    .font(.caption).foregroundStyle(.secondary)
                            }
                        }
                        .tag(step.id)
                    }
                    .onMove { steps.move(fromOffsets: $0, toOffset: $1) }
                }
                Divider()
                HStack {
                    Button { add() } label: { Image(systemName: "plus") }
                    Button { remove() } label: { Image(systemName: "minus") }.disabled(selected == nil)
                    Spacer()
                    Menu("Examples") {
                        Button("Move files to a library folder") { addExample(.move) }
                        Button("Encode with HandBrakeCLI") { addExample(.handbrake) }
                        Button("Rename for Plex with FileBot") { addExample(.filebot) }
                        Button("Log to a file") { addExample(.log) }
                        Button("Run a shell command") { addExample(.shell) }
                    }
                    .fixedSize()
                }
                .buttonStyle(.borderless)
                .padding(6)
            }
            .frame(minWidth: 200, idealWidth: 230, maxWidth: 300)

            Group {
                if let id = selected, let i = steps.firstIndex(where: { $0.id == id }) {
                    PostStepEditor(step: $steps[i], driveName: driveName)
                } else {
                    ContentUnavailableView {
                        Label("No step selected", systemImage: "terminal")
                    } description: {
                        Text(emptyText)
                    } actions: {
                        Button("Add Step") { add() }
                    }
                }
            }
            .frame(minWidth: 420, maxWidth: .infinity, maxHeight: .infinity)
        }
        .onAppear { if selected == nil { selected = steps.first?.id } }
    }

    static func conditionSummary(_ s: PostProcessStep) -> String {
        var parts: [String] = []
        if !s.matchName.trimmingCharacters(in: .whitespaces).isEmpty { parts.append("“\(s.matchName)”") }
        if !s.matchFormats.isEmpty { parts.append(s.matchFormats.joined(separator: ", ")) }
        return parts.isEmpty ? "" : " · " + parts.joined(separator: " · ")
    }

    private func add() {
        var s = PostProcessStep()
        s.name = "Step \(steps.count + 1)"
        steps.append(s)
        selected = s.id
    }

    private func remove() {
        steps.removeAll { $0.id == selected }
        selected = steps.first?.id
    }

    enum Example { case move, handbrake, filebot, log, shell }

    private func addExample(_ e: Example) {
        var s = PostProcessStep()
        switch e {
        case .move:
            s.name = "Move to library"
            s.executable = "/bin/mv"
            s.arguments = "-n {files} ~/Movies/Library/"
            s.failJobOnError = true
        case .handbrake:
            s.name = "Encode with HandBrake"
            s.executable = "/opt/homebrew/bin/HandBrakeCLI"
            s.arguments = "-i {file} -o \"{outputDir}/{stem}.mp4\" --preset \"Fast 1080p30\""
            s.perFile = true
            s.background = true
        case .filebot:
            s.name = "Rename for Plex with FileBot"
            s.executable = "/opt/homebrew/bin/filebot"
            s.arguments = "-rename {files} --db TheMovieDB --format \"{plex}\" --output ~/Media -non-strict --action copy"
        case .log:
            s.name = "Append to rip log"
            s.executable = "/bin/sh"
            s.arguments = "-c \"echo \\\"$(date) $BROMELIA_STATUS $BROMELIA_DISC_NAME $BROMELIA_OUTPUT_DIR\\\" >> ~/rips.log\""
            s.runOn = .always
        case .shell:
            s.name = "Shell command"
            s.executable = "/bin/zsh"
            s.arguments = "-c 'echo Ripped \"$BROMELIA_DISC_NAME\" to \"$BROMELIA_OUTPUT_DIR\"'"
        }
        steps.append(s)
        selected = s.id
    }
}

private struct PostStepEditor: View {
    @Binding var step: PostProcessStep
    let driveName: String
    @State private var testOutput: [String] = []
    @State private var testing = false

    var body: some View {
        Form {
            Section {
                TextField("Name", text: $step.name)
                Toggle("Enabled", isOn: $step.enabled)
                Picker("Run", selection: $step.runOn) {
                    ForEach(RunCondition.allCases) { Text($0.label).tag($0) }
                }
                Toggle("Run once for every produced file", isOn: $step.perFile)
                Toggle("Run in the background after the disc is ejected (encoding, uploads)", isOn: $step.background)
            }
            Section {
                TextField("Movie / show name or disc label matches", text: $step.matchName, prompt: Text("Any — regular expression, e.g. ^One Piece$ or S2_P7"))
                if let err = PluginMatcher.validate(step.matchName) { Text(err).font(.caption).foregroundStyle(.red) }
                LabeledContent("Formats") {
                    HStack {
                        ForEach(DiscFormat.allCodes.filter { !$0.hasPrefix("HDDVD") }, id: \.self) { code in
                            Toggle(code, isOn: Binding(
                                get: { step.matchFormats.contains(code) },
                                set: { on in
                                    step.matchFormats.removeAll { $0 == code }
                                    if on { step.matchFormats.append(code) }
                                }))
                            .toggleStyle(.button)
                        }
                    }
                }
            } header: {
                Text("Applies to")
            } footer: {
                Text("Leave both empty to run for every disc. The name is the movie or show name used for file names; the disc label (e.g. ONE_PIECE_S2_P7_D2) lets a step target one specific disc. No format selected = all formats; the e codes are backups that are not decrypted.")
                    .font(.caption).foregroundStyle(.secondary)
            }
            Section("Command") {
                PathField(title: "Program or script", path: $step.executable, kind: .file, placeholder: "/path/to/script.sh")
                TextField("Interpreter", text: $step.interpreter, prompt: Text("Optional, e.g. /usr/bin/python3"))
                TextField("Arguments", text: $step.arguments, prompt: Text("{outputDir}"))
                    .font(.system(.body, design: .monospaced))
                TextField("Working folder", text: $step.workingDirectory, prompt: Text("Default: the job's output folder"))
                LabeledContent("Time limit") {
                    IntField(title: "Timeout", value: $step.timeoutSeconds, suffix: "seconds (0 = none)")
                }
                Toggle("Mark the job as failed if this step fails", isOn: $step.failJobOnError)
                Text("Arguments are split like a shell command line and then {tokens} are filled in, so values with spaces stay a single argument. A lone {files} expands to one argument per file. No shell expansion is done unless you run a shell.")
                    .font(.caption).foregroundStyle(.secondary)
                TokenReference(tokens: TemplateRenderer.scriptTokens)
            }
            Section {
                KeyValueEditor(values: $step.environment, keyPlaceholder: "VARIABLE", valuePlaceholder: "value ({tokens} allowed)")
            } header: {
                Text("Environment")
            } footer: {
                Text("Always set: BROMELIA_JOB_ID, BROMELIA_STATUS, BROMELIA_MODE, BROMELIA_DRIVE_NAME, BROMELIA_DRIVE_ID, BROMELIA_DEVICE, BROMELIA_DISC_NAME, BROMELIA_DISC_TYPE, BROMELIA_OUTPUT_DIR, BROMELIA_FILES (newline separated), BROMELIA_FILE_COUNT, BROMELIA_FILE (per-file steps), BROMELIA_MANIFEST (JSON), BROMELIA_LOG, BROMELIA_SOURCE, BROMELIA_ERROR, BROMELIA_NAME, BROMELIA_KIND (movie / tv), BROMELIA_FORMAT (DVD, BRe, 4K, …), BROMELIA_ENCRYPTED (1 / 0), BROMELIA_SEASON, BROMELIA_DISC_NUMBER, BROMELIA_DISC_SET, BROMELIA_CHECKSUMS (SHA256SUMS path).")
                    .font(.caption).foregroundStyle(.secondary).textSelection(.enabled)
            }
            Section("Test") {
                HStack {
                    Button(testing ? "Running…" : "Run with Sample Values") { Task { await test() } }
                        .disabled(testing || step.executable.isEmpty)
                    Text("Runs the step now with a sample job (status success, no files).")
                        .font(.caption).foregroundStyle(.secondary)
                }
                if !testOutput.isEmpty {
                    ScrollView {
                        Text(testOutput.joined(separator: "\n"))
                            .font(.system(.caption, design: .monospaced))
                            .textSelection(.enabled)
                            .frame(maxWidth: .infinity, alignment: .leading)
                    }
                    .frame(height: 140)
                }
            }
        }
        .formStyle(.grouped)
    }

    private func test() async {
        testing = true
        testOutput = []
        let tmp = FileManager.default.temporaryDirectory.appendingPathComponent("bromelia-test", isDirectory: true)
        try? FileManager.default.createDirectory(at: tmp, withIntermediateDirectories: true)
        var values = TemplateRenderer.dateValues()
        values["disc"] = "SAMPLE_DISC"; values["volume"] = "SAMPLE_DISC"; values["type"] = "bd"; values["drive"] = driveName
        values["job"] = "test0000"; values["outputDir"] = tmp.path; values["status"] = "success"; values["manifest"] = ""
        values["file"] = ""; values["files"] = ""; values["device"] = ""; values["checksums"] = ""
        let id = MediaIdentity(name: "Sample Disc", kind: .movie, format: .bluray, encrypted: false, label: LabelInfo(), reason: "")
        for (k, v) in id.templateValues(rip: "Rip") { values[k] = v }
        let env = ["BROMELIA_STATUS": "success", "BROMELIA_DISC_NAME": "SAMPLE_DISC", "BROMELIA_OUTPUT_DIR": tmp.path,
                   "BROMELIA_DRIVE_NAME": driveName, "BROMELIA_FILES": "", "BROMELIA_FILE_COUNT": "0", "BROMELIA_MODE": "mkv",
                   "BROMELIA_NAME": id.name, "BROMELIA_KIND": "movie", "BROMELIA_FORMAT": id.formatCode, "BROMELIA_ENCRYPTED": "0"]
        var s = step
        s.runOn = .always
        s.enabled = true
        s.perFile = false
        let ctx = PostProcessor.Context(status: .succeeded, values: values, outputDirectory: tmp, files: [], manifestPath: "", environment: env)
        let collector = LineCollector()
        _ = await PostProcessor.run(steps: [s], context: ctx, register: { _ in }, log: { text, _ in collector.append(text) })
        testOutput = collector.all
        testing = false
    }
}
