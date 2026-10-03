/// templates: the profile's folder and file templates; mediaServer: the Plex / Jellyfin / Emby names.
public enum Layout: String, Sendable, CaseIterable {
    case templates
    case mediaServer
}
