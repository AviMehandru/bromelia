import BroDomain
import BroFoundation
import BroPorts
import Foundation

/// Episode numbers from a DVD's menu stills (plan §10.1; shared/fixtures/adapters/menu-ocr.cases.json): ffmpeg turns
/// each still cell into a large grey PNG, tesseract reads it, MenuNumbers.parse finds the numbers.
public final class MenuOcr: Sendable {
    private static let sector: Int64 = 2048
    private let launcher: any ProcessLauncher
    private let locator: any ToolLocator
    private let fs: any FileSystem

    public init(launcher: any ProcessLauncher, locator: any ToolLocator, fs: any FileSystem) {
        self.launcher = launcher
        self.locator = locator
        self.fs = fs
    }

    /// The PNG of each cell ffmpeg could convert, in order, in `dir` (which must exist).
    public func extractStills(_ source: ByteSource, cells: [CellRef], dir: String, cancel: CancellationToken) async throws(BroError) -> [String] {
        let ffmpeg = try tool(.ffmpeg)
        var stills: [String] = []
        for (i, cell) in cells.enumerated() {
            let mpg = (dir as NSString).appendingPathComponent("menu\(i).mpg"), png = (dir as NSString).appendingPathComponent("menu\(i).png")
            try fs.writeAtomically(mpg, bytes: source.read(cell.file, offset: cell.firstSector * Self.sector,
                                                           length: Int((cell.endSector - cell.firstSector) * Self.sector)), mode: 0o644)
            do {
                let (exit, _) = try await run(ffmpeg, ["-v", "quiet", "-y", "-f", "mpeg", "-i", mpg, "-frames:v", "1", "-vf", "scale=2160:1440,format=gray", png],
                                              cancel)
                if exit.status == 0 && fs.exists(png) { stills.append(png) }
            } catch {
                try? fs.remove(mpg)
                throw error
            }
            try? fs.remove(mpg)
        }
        return stills
    }

    /// The episode numbers on `stills`, each once, in the order first read.
    public func readNumbers(_ stills: [String], cancel: CancellationToken) async throws(BroError) -> [Int] {
        let tesseract = try tool(.tesseract)
        var numbers: [Int] = []
        for still in stills {
            let (exit, lines) = try await run(tesseract, [still, "stdout", "--psm", "11"], cancel)
            guard exit.status == 0 else { continue }
            for n in MenuNumbers.parse(lines.joined(separator: "\n")) where !numbers.contains(n) { numbers.append(n) }
        }
        return numbers
    }

    private func tool(_ tool: ToolKind) throws(BroError) -> String {
        let info = locator.locate(tool)
        guard let path = info.path else { throw (info.why ?? BroMessage(.toolMissing, [("tool", .string(tool.rawValue))], severity: .warning)).toError() }
        return path
    }

    private func run(_ exe: String, _ arguments: [String], _ cancel: CancellationToken) async throws(BroError) -> (ProcessExit, [String]) {
        if cancel.isCancelled { throw BroError(MessageCode.jobCancelled.rawValue) }
        var lines: [String] = []
        let exit = try await ToolRun.run(launcher, ProcessSpec(executable: exe, arguments: arguments, environment: [:], stopPolicy: .interruptFirst,
                                                               stallTimeout: Duration(seconds: 60)), cancel: cancel) { line in
            if line.stream == .stdout { lines.append(line.text) }
            return nil
        }
        if exit.cancelled { throw BroError(MessageCode.jobCancelled.rawValue) }
        return (exit, lines)
    }
}
