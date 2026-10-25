/// Every step a job can run (plan §20.1).
public enum StepKind: String, Sendable, CaseIterable, Codable {
    case awaitMedia
    case probe
    case reconcile
    case readNavigation
    case identify
    case plan
    case decide
    case acquire
    case release
    case verifyRips
    case transform
    case name
    case seal
    case commit
    case publish
    case protect
    case postProcess
    case notify
    /// Only after JobService.keepApart: an interrupted job's staging sealed and quarantined as INCOMPLETE (owner, 2026-10-25).
    case keepApart
    case ripAudio
    case imageData
    case mergeAttempts
    case verifyUnit
    case replicate
    case parity
    case repair
    case rescan
    case runCommand
    case transcode
}
