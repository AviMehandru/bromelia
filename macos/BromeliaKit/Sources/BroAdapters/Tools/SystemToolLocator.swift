import BroDomain
import BroFoundation
import BroPorts
import Foundation

/// ToolLocator (plan §9; shared/fixtures/adapters/tool-locator.cases.json): the configured path (when set, nothing
/// else is tried), else mkvextract next to the mkvmerge that was found, else the candidates in order, else each folder
/// of the search path with each program name in order. Versions and capabilities come from the tools themselves.
public final class SystemToolLocator: ToolLocator {
    private let fs: any FileSystem
    private let configured: [ToolKind: String]
    private let candidates: [ToolKind: [String]]
    private let names: [ToolKind: [String]]
    private let searchPath: [String]
    private let home: String

    /// - Parameters:
    ///   - configured: the paths set in the configuration (tools.*, makemkv.path); empty means unset.
    ///   - candidates: PlatformToolPaths.candidates: where each tool is usually installed.
    ///   - names: PlatformToolPaths.names: its program names, tried in order.
    ///   - searchPath: the folders of PATH.
    ///   - home: what "~/" stands for.
    public init(fs: any FileSystem, configured: [ToolKind: String], candidates: [ToolKind: [String]], names: [ToolKind: [String]],
                searchPath: [String], home: String) {
        self.fs = fs
        self.configured = configured
        self.candidates = candidates
        self.names = names
        self.searchPath = searchPath
        self.home = home
    }

    public func locate(_ tool: ToolKind) -> ToolInfo {
        if let set = configured[tool]?.trimmingCharacters(in: .whitespaces), !set.isEmpty {
            let path = expand(set)
            return isFile(path) ? found(tool, path)
                : ToolInfo(tool: tool, capabilities: [], why: BroMessage(.toolNotFoundAt, [("tool", .string(tool.rawValue)), ("path", .string(path))], severity: .error))
        }
        if tool == .mkvextract, let mkvmerge = locate(.mkvmerge).path {
            let dir = String(mkvmerge[..<(mkvmerge.lastIndex(of: "/") ?? mkvmerge.startIndex)])
            for name in programNames(tool) where isFile(dir + "/" + name) { return found(tool, dir + "/" + name) }
        }
        for c in candidates[tool] ?? [] where isFile(expand(c)) { return found(tool, expand(c)) }
        for folder in searchPath where !folder.isEmpty {
            for name in programNames(tool) where isFile(folder + "/" + name) { return found(tool, folder + "/" + name) }
        }
        return ToolInfo(tool: tool, capabilities: [], why: BroMessage(.toolMissing, [("tool", .string(tool.rawValue))], severity: .warning))
    }

    private func programNames(_ tool: ToolKind) -> [String] { names[tool] ?? [tool.rawValue] }

    private func found(_ tool: ToolKind, _ path: String) -> ToolInfo { ToolInfo(tool: tool, path: path, capabilities: []) }

    private func expand(_ path: String) -> String { path.hasPrefix("~/") ? home + "/" + path.dropFirst(2) : path }

    private func isFile(_ path: String) -> Bool { (try? fs.stat(path).isDirectory == false) ?? false }
}
