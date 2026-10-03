import Foundation

/// HandBrake steps: HandBrakeCLI with a preset, one encode per ripped MKV, next to the archive (never over it).
public enum HandBrakeArgs {
    /// `[--preset-import-file F] [--preset P] -i input -o output [more arguments]`. A leading ~ in the preset file is
    /// `home`.
    public static func build(_ step: StepDefinition, input: String, output: String, home: String?) -> [String] {
        let h = step.handbrake
        var args: [String] = []
        let file = CommandArgs.expandHome(h.presetFile.trimmingCharacters(in: .whitespaces), home)
        if !file.isEmpty { args += ["--preset-import-file", file] }
        let preset = h.preset.trimmingCharacters(in: .whitespaces)
        if !preset.isEmpty { args += ["--preset", preset] }
        args += ["-i", input, "-o", output]
        args += ArgumentSplitter.split(h.extraArguments)
        return args
    }

    /// The encode's path: the step's output path (default `{outputDir}/Encoded/{stem}.mkv`) filled in for the file;
    /// a leading ~ is `home`.
    public static func output(_ step: StepDefinition, values: [String: String], home: String?) -> String {
        let t = step.handbrake.outputPath.trimmingCharacters(in: .whitespaces)
        return CommandArgs.expandHome(TemplateEngine.render(t.isEmpty ? HandBrakeSettings().outputPath : t, values: values), home)
    }

    /// Only MKV files are encoded (not backups, ISO images or other files).
    public static func isSource(_ path: String) -> Bool { path.lowercased().hasSuffix(".mkv") }

    /// Keeps HandBrakeCLI's progress lines (`Encoding: task 1 of 1, 45.12 %`) at every 10 % of each task, and every
    /// other line.
    public static func keepLine(_ filter: inout ProgressFilter, line: String) -> Bool {
        let re = try! NSRegularExpression(pattern: #"Encoding: task (\d+) of \d+, (\d+)\.\d+ %"#)
        guard let m = re.firstMatch(in: line, range: NSRange(line.startIndex..., in: line)),
              let task = Int(line[Range(m.range(at: 1), in: line)!]), let percent = Int(line[Range(m.range(at: 2), in: line)!]) else { return true }
        if task != filter.task { filter = ProgressFilter(task: task, next: 0) }
        if percent / 10 < filter.next { return false }
        filter.next = percent / 10 + 1
        return true
    }
}
