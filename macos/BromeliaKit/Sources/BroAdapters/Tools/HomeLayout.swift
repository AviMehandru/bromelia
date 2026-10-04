/// Where makemkvcon reads settings.conf under HOME: Library/MakeMKV (macOS) or .MakeMKV (Linux).
public enum HomeLayout: String, Sendable, CaseIterable {
    case macos
    case linux
}
