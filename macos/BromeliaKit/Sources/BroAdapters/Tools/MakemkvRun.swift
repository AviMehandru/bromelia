import BroDomain

/// One run: its outcome, the first key, version or drive notice, the LibreDrive detail, and MakeMKV's version.
public struct MakemkvRun: Sendable, Equatable {
    public var outcome: RunOutcome
    public var notice: MakemkvNotice?
    public var libreDrive: String?
    public var version: String?

    public init(outcome: RunOutcome, notice: MakemkvNotice? = nil, libreDrive: String? = nil, version: String? = nil) {
        self.outcome = outcome
        self.notice = notice
        self.libreDrive = libreDrive
        self.version = version
    }
}
