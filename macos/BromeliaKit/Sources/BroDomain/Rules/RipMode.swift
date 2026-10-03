/// What a video-disc job makes (common.json's RipMode); audioCD and dataImage are chosen from the disc's content.
public enum RipMode: String, Sendable, CaseIterable {
    case mkv
    case backup
    case backupDecrypted
    case backupThenMkv
    case infoOnly
    case audioCD
    case dataImage
}
