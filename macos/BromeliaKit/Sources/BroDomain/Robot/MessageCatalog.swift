/// Known MakeMKV message codes → severity and notices.
public enum MessageCatalog {
    /// 2018: write error (e.g. "No space left on device"); 5006: the source file doesn't exist.
    static let errorCodes: Set<Int> = [2003, 2004, 2018, 2023, 5003, 5006, 5010, 5021, 5037, 5055, 5069, 5077]
    static let warningCodes: Set<Int> = [3038, 3041, 5042]

    /// Debug for code 1003 and debug-flagged "DEBUG…" text; error and warning by flag (0x200 / 0x400) or known
    /// code; info otherwise.
    public static func severity(_ message: RobotMessage) -> MessageKind {
        if message.code == 1003 || (message.flags & 0x20 != 0 && message.text.hasPrefix("DEBUG")) { return .debug }
        if message.flags & 0x200 != 0 || errorCodes.contains(message.code) { return .error }
        if message.flags & 0x400 != 0 || warningCodes.contains(message.code) { return .warning }
        return .info
    }

    public static func notice(_ message: RobotMessage) -> MakemkvNotice? {
        let t = message.text
        if t.hasPrefix("Using LibreDrive mode") {
            if let open = t.firstIndex(of: "("), let close = t.lastIndex(of: ")"), open < close {
                return .libreDrive(detail: String(t[t.index(after: open)..<close]))
            }
            return .libreDrive(detail: "")
        }
        if t.contains("LibreDrive compatible drive is required") { return .libreDriveRequired }
        let lower = asciiLower(t)
        if message.code == 5052 || message.code == 5055 || lower.contains("evaluation period has expired")
            || lower.contains("evaluation period expired") { return .keyExpired }
        if t.contains("Evaluation period not started") || t.contains("start MakeMKV evaluation from a third-party application") {
            return .evaluationNotStarted
        }
        if t.contains("application version is too old") { return .versionTooOld }
        return nil
    }

    /// ASCII case folding, as on the other platforms.
    static func asciiLower(_ s: String) -> String {
        String(String.UnicodeScalarView(s.unicodeScalars.map { $0.value >= 65 && $0.value <= 90 ? Unicode.Scalar($0.value + 32)! : $0 }))
    }
}
