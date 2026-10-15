import BroDomain
import BroFoundation
import BroPorts

/// A job's log file, <data>/jobs/<id>/log.jsonl (plan §16): one JSON object per line, {t, severity, code, params}, in the
/// canonical encoding written on one line, so every platform writes the same bytes.
public enum JobLog {
    public static let fileName = "log.jsonl"

    /// The lines, each ended by a line break (their seq is left out).
    public static func encode(_ lines: [LogLineDto]) -> [UInt8] {
        var text = ""
        for line in lines {
            guard case let .object(members) = LogLineDto.json(line) else { continue }
            text += oneLine(.object(members.filter { $0.key != "seq" })) + "\n"
        }
        return Array(text.utf8)
    }

    /// Adds the lines to the end of the job folder's log (the folder made when it isn't there), written atomically, 0600.
    public static func append(_ fs: any FileSystem, jobDirectory: String, lines: [LogLineDto]) throws(BroError) {
        if lines.isEmpty { return }
        let path = jobDirectory + "/" + fileName
        if !fs.exists(jobDirectory) { try fs.createDirectory(jobDirectory, parentsMustExist: false) }
        let before = fs.exists(path) ? try fs.read(path) : []
        try fs.writeAtomically(path, bytes: before + encode(lines), mode: 0o600)
    }

    /// The canonical encoding on one line: its line breaks and their indentation are formatting only (a string never
    /// holds a raw line break).
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
