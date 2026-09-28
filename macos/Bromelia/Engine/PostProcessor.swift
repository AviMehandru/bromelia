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
        guard step.enabled, !step.executable.trimmingCharacters(in: .whitespaces).isEmpty else { return false }
        switch step.runOn {
        case .always: return true
        case .success: return status == .succeeded
        case .failure: return status == .failed || status == .cancelled
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
            let targets: [URL?] = step.perFile ? (context.files.isEmpty ? [] : context.files.map { Optional($0) }) : [nil]
            for file in targets {
                if Task.isCancelled { return results }
                var values = context.values
                if let file {
                    values["file"] = file.path
                    values["filename"] = file.lastPathComponent
                    values["stem"] = file.deletingPathExtension().lastPathComponent
                }
                let (exe, args) = buildInvocation(step, values: values, files: context.files)
                let wdTemplate = step.workingDirectory.trimmingCharacters(in: .whitespaces)
                var wd = wdTemplate.isEmpty ? context.outputDirectory : URL(fileURLWithPath: Paths.expandTilde(TemplateRenderer.render(wdTemplate, values: values)))
                if let d = wd, !FileManager.default.fileExists(atPath: d.path) { wd = nil }
                let runner = ProcessRunner(executable: exe, arguments: args,
                                           environment: environment(for: step, context: context, file: file),
                                           workingDirectory: wd)
                let label = file.map { "\(step.name) (\($0.lastPathComponent))" } ?? step.name
                log("▶︎ \(label): \(runner.commandLine)", .info)
                register(runner)
                do {
                    let out = try await runner.run(timeout: TimeInterval(step.timeoutSeconds)) { line in
                        log("  [\(step.name)] \(line)", .info)
                    }
                    register(nil)
                    let sev: RobotMessage.Severity = out.exitCode == 0 ? .info : (step.failJobOnError ? .error : .warning)
                    log(out.timedOut ? "\(label) timed out after \(step.timeoutSeconds) s" : "\(label) exited with status \(out.exitCode)", out.timedOut ? .warning : sev)
                    results.append(StepResult(stepId: step.id, name: step.name, exitCode: out.exitCode, timedOut: out.timedOut))
                } catch {
                    register(nil)
                    log("\(label) could not be started: \(error.localizedDescription)", step.failJobOnError ? .error : .warning)
                    results.append(StepResult(stepId: step.id, name: step.name, exitCode: -1, timedOut: false))
                }
            }
        }
        return results
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
