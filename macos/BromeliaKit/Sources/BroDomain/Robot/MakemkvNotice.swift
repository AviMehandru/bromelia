/// Messages about the drive and about MakeMKV itself that Bromelia shows outside the log.
public enum MakemkvNotice: Sendable, Equatable {
    /// "Using LibreDrive mode (v06.3 id=…)": the drive reads the disc in LibreDrive mode.
    case libreDrive(detail: String)
    /// The disc (4K UHD) can only be decrypted by a LibreDrive-compatible drive, and this one isn't.
    case libreDriveRequired
    /// The evaluation period or beta key has expired (messages 5052 and 5055).
    case keyExpired
    /// The evaluation hasn't been started; makemkvcon can't start it.
    case evaluationNotStarted
    /// "This application version is too old": MakeMKV needs updating, or a purchased key.
    case versionTooOld

    /// MakeMKV can't (fully) work until the user acts: a key, an update or starting the evaluation.
    public static func isLicenseProblem(_ notice: MakemkvNotice) -> Bool {
        switch notice {
        case .keyExpired, .evaluationNotStarted, .versionTooOld: return true
        case .libreDrive, .libreDriveRequired: return false
        }
    }
}
