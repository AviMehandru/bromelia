import BroDomain
import BroFoundation
import BroPorts
import Foundation

/// Data discs to ISO images (plan §10.1; shared/fixtures/adapters/data-imager.cases.json): every sector of
/// DriveControl.openRaw, 1 MiB at a time, into a hidden file that becomes the image only once every sector was read.
/// A read error names the first byte that can't be read (other.readError).
public final class DataImager: Sendable {
    private static let sector = 2048, chunk = 512, max = 10000
    private let drives: any DriveControl
    private let fs: any FileSystem

    public init(drives: any DriveControl, fs: any FileSystem) {
        self.drives = drives
        self.fs = fs
    }

    /// Copies the disc in `device` to `destIso` (which mustn't exist; its folder must) and returns the bytes copied. The
    /// copy runs on a thread of its own.
    public func copy(_ device: String, destIso: String, sink: any RunSink, cancel: CancellationToken) async throws(BroError) -> Int64 {
        let result: Result<Int64, BroError> = await withCheckedContinuation { c in
            Thread.detachNewThread { [self] in
                do throws(BroError) { c.resume(returning: .success(try copyNow(device, destIso, sink, cancel))) } catch { c.resume(returning: .failure(error)) }
            }
        }
        return try result.get()
    }

    private func copyNow(_ device: String, _ destIso: String, _ sink: any RunSink, _ cancel: CancellationToken) throws(BroError) -> Int64 {
        if fs.exists(destIso) { throw BroMessage(.fsAlreadyExists, [("path", .string(destIso))], severity: .error).toError() }
        if cancel.isCancelled { throw BroError(MessageCode.jobCancelled.rawValue) }
        let reader = try drives.openRaw(device)
        let folder = (destIso as NSString).deletingLastPathComponent
        let part = (folder as NSString).appendingPathComponent(".\((destIso as NSString).lastPathComponent).part-\(UUID().uuidString.prefix(8).lowercased())")
        var done = false
        defer {
            reader.close()
            if !done { unlink(part) }
        }
        let total = try reader.sectorCount()
        var copied: Int64 = 0
        let fd = open(part, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0o644)
        guard fd >= 0 else { throw failed(part, String(cString: strerror(errno))) }
        do {
            defer { close(fd) }
            while copied < total {
                if cancel.isCancelled { throw BroError(MessageCode.jobCancelled.rawValue) }
                let count = Int(min(Int64(Self.chunk), total - copied))
                let data = try readChunk(reader, copied, count)
                var written = 0
                while written < data.count {
                    let n = data.withUnsafeBytes { write(fd, $0.baseAddress! + written, data.count - written) }
                    if n < 0 && errno == EINTR { continue }
                    guard n > 0 else { throw failed(part, String(cString: strerror(errno))) }
                    written += n
                }
                copied += Int64(count)
                let share = Int(copied * Int64(Self.max) / total)
                sink.event(.progressValue(current: share, total: share, max: Self.max))
            }
        }
        try fs.syncFile(part)
        if cancel.isCancelled { throw BroError(MessageCode.jobCancelled.rawValue) }
        try fs.rename(part, to: destIso)
        try fs.syncDirectory(folder)
        done = true
        return copied * Int64(Self.sector)
    }

    /// A chunk; when it can't be read whole, its sectors one by one, so the error names the first bad byte (a short read
    /// is an error too: the image is never shorter than the disc).
    private func readChunk(_ reader: any SectorReader, _ sector: Int64, _ count: Int) throws(BroError) -> [UInt8] {
        if let whole = try? reader.read(sector, count: count), whole.count == count * Self.sector { return whole }
        var data: [UInt8] = []
        data.reserveCapacity(count * Self.sector)
        for i in 0..<count {
            let at = (sector + Int64(i)) * Int64(Self.sector)
            let one: [UInt8]
            do throws(BroError) { one = try reader.read(sector + Int64(i), count: 1) } catch {
                throw readError(at, JsonValue.object(error.params)["reason"]?.string ?? error.code)
            }
            guard one.count == Self.sector else { throw readError(at, "short read") }
            data += one
        }
        return data
    }

    private func readError(_ offset: Int64, _ reason: String) -> BroError {
        BroMessage(.otherReadError, [("offset", .integer(offset)), ("reason", .string(reason))], severity: .error).toError()
    }

    private func failed(_ path: String, _ reason: String) -> BroError {
        BroMessage(.fsFailed, [("operation", .string("write")), ("path", .string(path)), ("reason", .string(reason))], severity: .error).toError()
    }
}
