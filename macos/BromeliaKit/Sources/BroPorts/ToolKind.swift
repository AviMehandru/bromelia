/// The external tools Bromelia runs.
public enum ToolKind: String, Sendable, CaseIterable {
    case makemkvcon
    case mkvmerge
    case mkvextract
    case ffmpeg
    case tesseract
    case handbrake
    case cyanrip
    case abcde
    case par2
    case apprise
}
