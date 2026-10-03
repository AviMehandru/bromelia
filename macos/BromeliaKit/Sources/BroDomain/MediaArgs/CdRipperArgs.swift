import BroFoundation
import Foundation

/// The audio CD command.
public enum CdRipperArgs {
    /// The profile's otherDiscs.audioCommand ({device} is the drive; its program is resolved by the adapter), else
    /// cyanrip, else abcde, when `available` (the tools found) has it; nil when there is none. The command runs in
    /// the output folder.
    public static func build(_ otherDiscs: JsonValue, device: String, available: [String]) -> CommandLine? {
        let custom = (otherDiscs["audioCommand"]?.string ?? "").trimmingCharacters(in: .whitespaces)
        if !custom.isEmpty {
            let parts = ArgumentSplitter.split(custom).map { TemplateEngine.render($0, values: ["device": device]) }
            guard let first = parts.first else { return nil }
            return CommandLine(executable: first, arguments: Array(parts.dropFirst()))
        }
        if available.contains("cyanrip") { return CommandLine(executable: "cyanrip", arguments: ["-d", device, "-o", "flac"]) }
        if available.contains("abcde") { return CommandLine(executable: "abcde", arguments: ["-d", device, "-o", "flac", "-N"]) }
        return nil
    }
}
