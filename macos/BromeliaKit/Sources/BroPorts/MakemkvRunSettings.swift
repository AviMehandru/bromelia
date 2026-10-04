import BroDomain
import BroFoundation

/// One makemkvcon run's settings: settings.conf (SettingsLayers.merge), the generated profile (ProfileXml.render; none:
/// no profile file), the user's MakeMKV data folder, and the run's own folder (the job's home/: HOME, the profile file).
public struct MakemkvRunSettings: Sendable, Equatable {
    public var settings: [String: String]
    public var profileXml: String?
    public var dataDir: String
    public var workDirectory: String

    public init(settings: [String: String], profileXml: String? = nil, dataDir: String, workDirectory: String) {
        self.settings = settings
        self.profileXml = profileXml
        self.dataDir = dataDir
        self.workDirectory = workDirectory
    }
}
