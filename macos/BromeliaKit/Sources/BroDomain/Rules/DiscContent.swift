/// What is on a disc (DriveControl.probeContent).
public enum DiscContent: String, Sendable, CaseIterable {
    case video
    case audio
    case data
    case blank
    case unknown
}
