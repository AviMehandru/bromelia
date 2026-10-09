// BroTestProbe <mode> <folder>: started by BroAdaptersTests' CrashTests (the same points as Linux's /crash/* and
// Windows' CrashTests). "Killed" is SIGKILL on itself: no defer, no deinit, like a crash or a power cut.
//   image <folder>                    a DataImager copy into <folder>, killed after two of four chunks
//   move-before|move-after <folder>   moveMerging of <folder>/from into <folder>/to, killed around its third report,
//                                     just before or just after it; reports go to <folder>/report.txt
//   tool-then-die <folder>            starts a tool with PlatformProcessLauncher (a shell with a background child,
//                                     their pids in <folder>/tool.pid and child.pid), killed while it runs
//   tool-exits-then-die <folder>      the same tool without its wait: it exits (its child stays), then the probe is killed
import BroAdapters
import BroDomain
import BroFoundation
import BroPorts
import Foundation

@main
struct Probe {
    static func main() async {
        let args = Swift.CommandLine.arguments
        guard args.count == 3 else { exit(2) }
        let folder = args[2]
        switch args[1] {
        case "image":
            let drives = Drives()
            _ = try? await DataImager(drives: drives, fs: PlatformFileSystem()).copy("/dev/rdisk9", destIso: folder + "/disc.iso", sink: NoSink(),
                                                                                     cancel: CancellationSource().token)
        case "move-before", "move-after":
            let report = folder + "/report.txt"
            FileManager.default.createFile(atPath: report, contents: nil)
            var seen = 0
            _ = try? PlatformFileSystem().moveMerging(folder + "/from", to: folder + "/to", policy: .neverReplace) { item in
                seen += 1
                if seen == 3 && args[1] == "move-before" { die() }
                let h = FileHandle(forWritingAtPath: report)!
                h.seekToEndOfFile()
                h.write(Data("\(item.from)\t\(item.to)\n".utf8))
                try? h.close()
                if seen == 3 { die() }
            }
        case "tool-then-die", "tool-exits-then-die":
            let runs = args[1] == "tool-then-die"
            let script = "sleep 60 & echo $! > child.pid; echo $$ > tool.pid" + (runs ? "; exec sleep 60" : "")
            let spec = ProcessSpec(executable: "/bin/sh", arguments: ["-c", script], environment: [:], workingDirectory: folder,
                                   stopPolicy: .terminateFirst)
            guard let tool = try? PlatformProcessLauncher().start(spec) else { exit(1) }
            if runs {
                for _ in 0..<100 where !FileManager.default.fileExists(atPath: folder + "/child.pid") { usleep(50_000) }
            } else {
                _ = await tool.wait()
            }
            die()
        default:
            exit(2)
        }
        exit(1) // not killed: the test fails
    }
}

func die() -> Never {
    kill(getpid(), SIGKILL)
    while true { pause() }
}

struct NoSink: RunSink { func event(_ event: RobotEvent) {} }

/// Four chunks of zeros; the third read kills the process.
final class Disc: SectorReader, @unchecked Sendable {
    var reads = 0
    func read(_ sector: Int64, count: Int) throws(BroError) -> [UInt8] {
        reads += 1
        if reads == 3 { die() }
        return [UInt8](repeating: 0, count: Int(max(0, min(Int64(count), 4 * 512 - sector))) * 2048)
    }
    func sectorCount() throws(BroError) -> Int64 { 4 * 512 }
    func close() {}
}

final class Drives: DriveControl, @unchecked Sendable {
    let disc = Disc()
    func eject(_ device: String) async throws(BroError) {}
    func closeTray(_ device: String) async throws(BroError) {}
    func waitForMount(_ device: String, timeout: Duration, cancel: CancellationToken) async -> String? { nil }
    func probeContent(_ device: String) -> DiscContent { .unknown }
    func openRaw(_ device: String) throws(BroError) -> any SectorReader { disc }
}
