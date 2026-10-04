import BroDomain
import BroFoundation

/// One makemkvcon run's settings: settings.conf (SettingsLayers.merge), the generated profile (ProfileXml.render; none:
/// no profile file) and the user's MakeMKV data folder.
public struct MakemkvRunSettings: Sendable, Equatable {
    public var settings: [String: String]
    public var profileXml: String?
    public var dataDir: String

    public init(settings: [String: String], profileXml: String? = nil, dataDir: String) {
        self.settings = settings
        self.profileXml = profileXml
        self.dataDir = dataDir
    }
}
