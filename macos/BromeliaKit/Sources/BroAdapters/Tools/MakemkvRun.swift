import BroDomain

/// One run: its outcome, the first key, version or drive notice, the LibreDrive detail, MakeMKV's version,
/// process.noTranscript when its transcript couldn't be written, and what giving its settings back reported
/// (IsolationLease.release: makemkv.keyNotRemoved, makemkv.registryNotRestored).
public struct MakemkvRun: Sendable, Equatable {
    public var outcome: RunOutcome
    public var notice: MakemkvNotice?
    public var libreDrive: String?
    public var version: String?
    public var transcriptProblem: BroMessage?
    public var settingsProblem: BroMessage?

    public init(outcome: RunOutcome, notice: MakemkvNotice? = nil, libreDrive: String? = nil, version: String? = nil,
                transcriptProblem: BroMessage? = nil, settingsProblem: BroMessage? = nil) {
        self.outcome = outcome
        self.notice = notice
        self.libreDrive = libreDrive
        self.version = version
        self.transcriptProblem = transcriptProblem
        self.settingsProblem = settingsProblem
    }
}
