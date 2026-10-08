// BroTestProbe <mode> <folder>: started by BroAdaptersTests' CrashTests (the same points as Linux's /crash/* and
// Windows' CrashTests). "Killed" is SIGKILL on itself: no defer, no deinit, like a crash or a power cut.
//   image <folder>                    a DataImager copy into <folder>, killed after two of four chunks
//   move-before|move-after <folder>   moveMerging of <folder>/from into <folder>/to, killed around its third report,
//                                     just before or just after it; reports go to <folder>/report.txt
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
