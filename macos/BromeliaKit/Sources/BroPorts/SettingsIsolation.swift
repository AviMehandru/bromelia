import BroDomain
import BroFoundation

/// Gives each makemkvcon run its own settings.
public protocol SettingsIsolation: Sendable {
    func prepare(_ settings: MakemkvRunSettings) throws(BroError) -> any IsolationLease
}
