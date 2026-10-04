import BroPorts
import Foundation

/// Where each tool is usually installed on macOS, and its program names (today's candidate lists, Homebrew on Apple
/// silicon and Intel, the app bundles, MKVToolNix's versioned bundles).
public enum PlatformToolPaths {
    public static func candidates(_ home: String) -> [ToolKind: [String]] {
        let brew = ["/opt/homebrew/bin", "/usr/local/bin"]
        func inBrew(_ name: String) -> [String] { brew.map { "\($0)/\(name)" } }
        // MKVToolNix-89.0.app and the like, newest name last in the listing order sorted.
        let versioned = ((try? FileManager.default.contentsOfDirectory(atPath: "/Applications")) ?? [])
            .filter { $0.hasPrefix("MKVToolNix") && $0.hasSuffix(".app") }.sorted()
        func mkvToolNix(_ name: String) -> [String] {
            inBrew(name) + ["/Applications/MKVToolNix.app/Contents/MacOS/\(name)", "\(home)/Applications/MKVToolNix.app/Contents/MacOS/\(name)"]
                + versioned.map { "/Applications/\($0)/Contents/MacOS/\(name)" }
        }
        return [
            .makemkvcon: ["/Applications/MakeMKV.app/Contents/MacOS/makemkvcon", "\(home)/Applications/MakeMKV.app/Contents/MacOS/makemkvcon"]
                + inBrew("makemkvcon"),
            .mkvmerge: mkvToolNix("mkvmerge"),
            .mkvextract: mkvToolNix("mkvextract"),
            .handbrake: inBrew("HandBrakeCLI") + ["/Applications/HandBrakeCLI"],
            .ffmpeg: inBrew("ffmpeg"),
            .tesseract: inBrew("tesseract"),
            .cyanrip: inBrew("cyanrip"),
            .abcde: inBrew("abcde"),
            .par2: inBrew("par2"),
            .apprise: inBrew("apprise"),
        ]
    }

    public static func names() -> [ToolKind: [String]] {
        var n: [ToolKind: [String]] = [:]
        for tool in ToolKind.allCases { n[tool] = [tool == .handbrake ? "HandBrakeCLI" : tool.rawValue] }
        return n
    }
}
