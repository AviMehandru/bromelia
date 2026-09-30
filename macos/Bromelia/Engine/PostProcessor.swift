import Foundation

/// Runs a drive's post-processing steps after a job.
enum PostProcessor {
    struct Context: Sendable {
        var status: JobState
        var values: [String: String]          // template values (disc, drive, date, ...)
        var outputDirectory: URL?
        var files: [URL]
        var manifestPath: String
        var environment: [String: String]     // BROMELIA_* variables
    }

    struct StepResult: Sendable {
        var stepId: UUID
        var name: String
        var exitCode: Int32
        var timedOut: Bool
    }

    static func shouldRun(_ step: PostProcessStep, status: JobState) -> Bool {
        guard step.enabled, step.kind == .handbrake || !step.executable.trimmingCharacters(in: .whitespaces).isEmpty else { return false }
        switch step.runOn {
        case .always: return true
        case .success: return status == .succeeded
        case .failure: return status == .failed || status == .cancelled || status == .completedWithErrors
        }
    }

    /// Builds argv for a step. Arguments are split first and rendered afterwards, so values
    /// containing spaces stay a single argument. A lone `{files}` argument expands to one argument per file.
    static func buildInvocation(_ step: PostProcessStep, values: [String: String], files: [URL]) -> (URL, [String]) {
        var args: [String] = []
        for token in ArgumentSplitter.split(step.arguments) {
            if token == "{files}" {
                args += files.map(\.path)
            } else {
                args.append(TemplateRenderer.render(token, values: values))
            }
        }
        let exe = Paths.expandTilde(TemplateRenderer.render(step.executable, values: values))
        let interp = Paths.expandTilde(step.interpreter.trimmingCharacters(in: .whitespaces))
        if !interp.isEmpty {
            return (URL(fileURLWithPath: interp), [exe] + args)
        }
        if !FileManager.default.isExecutableFile(atPath: exe) {
            // Not executable (e.g. a script without the x bit): run it through the shell.
            return (URL(fileURLWithPath: "/bin/sh"), [exe] + args)
        }
        return (URL(fileURLWithPath: exe), args)
    }

    static func environment(for step: PostProcessStep, context: Context, file: URL?) -> [String: String] {
        var env = ProcessInfo.processInfo.environment
        for (k, v) in context.environment { env[k] = v }
        if let file { env["BROMELIA_FILE"] = file.path }
        for (k, v) in step.environment { env[k] = TemplateRenderer.render(v, values: context.values) }
        return env
    }

    /// Runs all applicable steps. `log` receives output lines. Returns results for each executed step.
    static func run(steps: [PostProcessStep], context: Context,
                    register: @escaping (ProcessRunner?) -> Void,
                    log: @escaping @Sendable (String, RobotMessage.Severity) -> Void) async -> [StepResult] {
        var results: [StepResult] = []
        for step in steps where shouldRun(step, status: context.status) {
            if step.kind == .handbrake {
                let sources = context.files.filter(HandBrake.isSource)
                guard let exe = HandBrake.tool(step) else {
                    log("\(step.name): HandBrakeCLI was not found. Install HandBrake's command line version (brew install handbrake), or set its location in the step.",
                        step.failJobOnError ? .error : .warning)
                    results.append(StepResult(stepId: step.id, name: step.name, exitCode: -1, timedOut: false))
                    continue
                }
                if sources.isEmpty { log("\(step.name): no MKV files to transcode", .info) }
                for file in sources {
                    if Task.isCancelled { return results }
                    let values = fileValues(context.values, file)
                    let output = Paths.uniqueURL(HandBrake.output(step, values: values))
                    do {
                        try FileManager.default.createDirectory(at: output.deletingLastPathComponent(), withIntermediateDirectories: true)
                    } catch {
                        log("\(step.name): can't create \(output.deletingLastPathComponent().path): \(error.localizedDescription)", step.failJobOnError ? .error : .warning)
                        results.append(StepResult(stepId: step.id, name: step.name, exitCode: -1, timedOut: false))
                        continue
                    }
                    let keep = HandBrake.ProgressFilter()
                    results.append(await invoke(step, label: "\(step.name) (\(file.lastPathComponent) → \(output.lastPathComponent))", exe: exe,
                                                args: HandBrake.arguments(step, input: file, output: output),
                                                environment: environment(for: step, context: context, file: file),
                                                workingDirectory: output.deletingLastPathComponent(), filter: keep.keep, register: register, log: log))
                }
                continue
            }
            let targets: [URL?] = step.perFile ? (context.files.isEmpty ? [] : context.files.map { Optional($0) }) : [nil]
            for file in targets {
                if Task.isCancelled { return results }
                let values = file.map { fileValues(context.values, $0) } ?? context.values
                let (exe, args) = buildInvocation(step, values: values, files: context.files)
                let wdTemplate = step.workingDirectory.trimmingCharacters(in: .whitespaces)
                var wd = wdTemplate.isEmpty ? context.outputDirectory : URL(fileURLWithPath: Paths.expandTilde(TemplateRenderer.render(wdTemplate, values: values)))
                if let d = wd, !FileManager.default.fileExists(atPath: d.path) { wd = nil }
                results.append(await invoke(step, label: file.map { "\(step.name) (\($0.lastPathComponent))" } ?? step.name, exe: exe, args: args,
                                            environment: environment(for: step, context: context, file: file), workingDirectory: wd,
                                            filter: nil, register: register, log: log))
            }
        }
        return results
    }

    /// The template values for one file of a per-file step.
    private static func fileValues(_ base: [String: String], _ file: URL) -> [String: String] {
        var values = base
        values["file"] = file.path
        values["filename"] = file.lastPathComponent
        values["stem"] = file.deletingPathExtension().lastPathComponent
        return values
    }

    /// Runs one command of a step and logs its output (lines `filter` rejects are left out).
    private static func invoke(_ step: PostProcessStep, label: String, exe: URL, args: [String], environment: [String: String], workingDirectory: URL?,
                               filter: (@Sendable (String) -> Bool)?, register: @escaping (ProcessRunner?) -> Void,
                               log: @escaping @Sendable (String, RobotMessage.Severity) -> Void) async -> StepResult {
        let runner = ProcessRunner(executable: exe, arguments: args, environment: environment, workingDirectory: workingDirectory)
        log("▶︎ \(label): \(runner.commandLine)", .info)
        register(runner)
        do {
            let out = try await runner.run(timeout: TimeInterval(step.timeoutSeconds)) { line in
                if filter?(line) ?? true { log("  [\(step.name)] \(line)", .info) }
            }
            register(nil)
            let sev: RobotMessage.Severity = out.exitCode == 0 ? .info : (step.failJobOnError ? .error : .warning)
            log(out.timedOut ? "\(label) timed out after \(step.timeoutSeconds) s" : "\(label) exited with status \(out.exitCode)", out.timedOut ? .warning : sev)
            return StepResult(stepId: step.id, name: step.name, exitCode: out.exitCode, timedOut: out.timedOut)
        } catch {
            register(nil)
            log("\(label) could not be started: \(error.localizedDescription)", step.failJobOnError ? .error : .warning)
            return StepResult(stepId: step.id, name: step.name, exitCode: -1, timedOut: false)
        }
    }
}

/// Presets built into HandBrake 1.6 and later, offered in the step editor (any preset name works).
enum HandBrakePresets {
    static let common = ["H.265 MKV 1080p30", "H.265 MKV 2160p60 4K", "H.264 MKV 1080p30", "H.264 MKV 480p30",
                         "Fast 1080p30", "HQ 1080p30 Surround", "Super HQ 1080p30 Surround", "Fast 2160p60 4K HEVC"]
}

/// HandBrake steps: HandBrakeCLI with a preset, one encode per ripped MKV, next to the archive (never over it).
enum HandBrake {
    /// The HandBrakeCLI of the step, or the one installed.
    static func tool(_ step: PostProcessStep) -> URL? {
        let exe = Paths.expandTilde(step.executable.trimmingCharacters(in: .whitespaces))
        if !exe.isEmpty { return FileManager.default.isExecutableFile(atPath: exe) ? URL(fileURLWithPath: exe) : nil }
        return EpisodeSplitter.findTool("HandBrakeCLI")
    }

    /// Only MKV files are transcoded (not backups, ISO images or other files).
    static func isSource(_ file: URL) -> Bool {
        var isDir: ObjCBool = false
        return file.pathExtension.lowercased() == "mkv" && (!FileManager.default.fileExists(atPath: file.path, isDirectory: &isDir) || !isDir.boolValue)
    }

    /// The encode's path: the step's output path rendered for the file (the default is `{outputDir}/Encoded/{stem}.mkv`).
    static func output(_ step: PostProcessStep, values: [String: String]) -> URL {
        let t = step.outputPath.trimmingCharacters(in: .whitespaces)
        return URL(fileURLWithPath: Paths.expandTilde(TemplateRenderer.render(t.isEmpty ? PostProcessStep.defaultEncodePath : t, values: values)))
    }

    /// `[--preset-import-file F] --preset P -i input -o output [extra arguments]`.
    static func arguments(_ step: PostProcessStep, input: URL, output: URL) -> [String] {
        var args: [String] = []
        let file = Paths.expandTilde(step.presetFile.trimmingCharacters(in: .whitespaces))
        if !file.isEmpty { args += ["--preset-import-file", file] }
        let preset = step.preset.trimmingCharacters(in: .whitespaces)
        if !preset.isEmpty { args += ["--preset", preset] }
        args += ["-i", input.path, "-o", output.path]
        return args + ArgumentSplitter.split(step.extraArguments)
    }

    /// Keeps HandBrakeCLI's progress lines (`Encoding: task 1 of 1, 45.12 %`) at every 10 % of each task, and every
    /// other line.
    final class ProgressFilter: @unchecked Sendable {
        private let lock = NSLock()
        private var task = -1, last = -1

        func keep(_ line: String) -> Bool {
            guard let m = line.firstMatch(of: /Encoding: task (\d+) of \d+, (\d+)\.\d+ %/), let t = Int(m.1), let p = Int(m.2) else { return true }
            lock.lock()
            defer { lock.unlock() }
            if t != task { task = t; last = -1 }
            if p / 10 <= last { return false }
            last = p / 10
            return true
        }
    }
}

/// Uses mkvmerge to keep only chosen tracks of a file ripped with every track selected.
enum Remuxer {
    struct TrackLayout { var id: Int; var type: String }

    static func identify(mkvmerge: URL, file: URL) async -> [TrackLayout]? {
        let collector = LineCollector()
        let runner = ProcessRunner(executable: mkvmerge, arguments: ["-J", file.path])
        guard let out = try? await runner.run(timeout: 120, onLine: { collector.append($0) }), out.exitCode == 0 else { return nil }
        guard let data = collector.joined.data(using: .utf8),
              let obj = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let tracks = obj["tracks"] as? [[String: Any]] else { return nil }
        return tracks.compactMap { t in
            guard let id = t["id"] as? Int, let type = t["type"] as? String else { return nil }
            return TrackLayout(id: id, type: type)
        }
    }

    static func expectedType(_ k: TrackInfo.Kind) -> String {
        switch k {
        case .video: return "video"
        case .audio: return "audio"
        case .subtitle: return "subtitles"
        default: return "other"
        }
    }

    /// Returns mkvmerge arguments, or nil when the file layout does not match the disc's track list.
    static func arguments(layout: [TrackLayout], title: TitleInfo, keep: Set<Int>, input: URL, output: URL) -> [String]? {
        guard layout.count == title.tracks.count else { return nil }
        for (l, t) in zip(layout, title.tracks) where expectedType(t.kind) != l.type && t.kind != .other {
            return nil
        }
        var video: [Int] = [], audio: [Int] = [], subs: [Int] = []
        for (l, t) in zip(layout, title.tracks) where keep.contains(t.index) {
            switch l.type {
            case "video": video.append(l.id)
            case "audio": audio.append(l.id)
            case "subtitles": subs.append(l.id)
            default: break
            }
        }
        func list(_ a: [Int]) -> String { a.map(String.init).joined(separator: ",") }
        var args = ["-o", output.path]
        args += video.isEmpty ? ["--no-video"] : ["--video-tracks", list(video)]
        args += audio.isEmpty ? ["--no-audio"] : ["--audio-tracks", list(audio)]
        args += subs.isEmpty ? ["--no-subtitles"] : ["--subtitle-tracks", list(subs)]
        args.append(input.path)
        return args
    }
}

final class LineCollector: @unchecked Sendable {
    private let lock = NSLock()
    private var lines: [String] = []
    func append(_ l: String) { lock.lock(); lines.append(l); lock.unlock() }
    var joined: String { lock.lock(); defer { lock.unlock() }; return lines.joined(separator: "\n") }
    var all: [String] { lock.lock(); defer { lock.unlock() }; return lines }
}
