import BroDomain
import BroFoundation
import BroPorts
import Foundation

/// The real EngineLog (plan §16; shared/fixtures/adapters/engine-log.cases.json): each line appended to
/// <data>/logs/engine.log through the FileSystem port, the objects a job's log.jsonl holds ({t, severity, code, params}, t
/// this clock's time at the call, params left out when empty) in the canonical encoding on one line. The logs folder is
/// made when it isn't there; the file is 0600. A line that can't be written is dropped. Rotation and the redaction of
/// secret values come with bromeliad (phase 4).
public final class FileEngineLog: EngineLog, @unchecked Sendable {
    private let fs: any FileSystem
    private let clock: any Clock
    private let path: String
    private let lock = NSLock()

    /// - Parameter path: <data>/logs/engine.log.
    public init(fs: any FileSystem, clock: any Clock, path: String) {
        self.fs = fs
        self.clock = clock
        self.path = path
    }

    public func log(_ message: BroMessage) {
        var members: [(key: String, value: JsonValue)] = [("t", .string(Instant.format(clock.now()))), ("severity", .string(message.severity.rawValue)),
                                                          ("code", .string(message.code.rawValue))]
        if !message.params.isEmpty { members.append(("params", .object(message.params))) }
        let line = Array((Self.oneLine(.object(members)) + "\n").utf8)
        let folder = (path as NSString).deletingLastPathComponent
        lock.withLock {
            do {
                if !folder.isEmpty && !fs.exists(folder) { try fs.createDirectory(folder, parentsMustExist: false) }
                let before = fs.exists(path) ? try fs.read(path) : []
                try fs.writeAtomically(path, bytes: before + line, mode: 0o600)
            } catch {}
        }
    }

    /// The canonical encoding on one line (as JobLog writes): its line breaks and their indentation are formatting only
    /// (a string never holds a raw line break).
    static func oneLine(_ value: JsonValue) -> String {
        let canonical = Array(JsonValue.encodeCanonical(value))
        var out: [UInt8] = []
        var i = 0
        while i < canonical.count {
            if canonical[i] == UInt8(ascii: "\n") {
                while i + 1 < canonical.count && canonical[i + 1] == UInt8(ascii: " ") { i += 1 }
            } else {
                out.append(canonical[i])
            }
            i += 1
        }
        return String(decoding: out, as: UTF8.self)
    }
}
