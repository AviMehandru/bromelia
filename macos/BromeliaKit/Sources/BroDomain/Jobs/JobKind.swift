public enum JobKind: String, Sendable, CaseIterable, Codable {
    case videoDisc
    case audioCd
    case dataDisc
    case readErrorRetry
    case verify
    case replicate
    case parity
    case repair
    case rescan
    case runCommand
    case transcode
}
