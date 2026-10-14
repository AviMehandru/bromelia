import BroDomain
import BroFoundation

/// Gives each makemkvcon run its own settings.
public protocol SettingsIsolation: Sendable {
    func prepare(_ settings: MakemkvRunSettings) throws(BroError) -> any IsolationLease

    /// At startup, for the runs the engine never released (it stopped while they ran): a HOME folder loses the
    /// registration key line of its settings.conf; the registry gets back the user's values a crash left in the snapshot
    /// file. `workDirectories` are those runs' folders (the unfinished jobs' home/). What it couldn't do, as warnings
    /// (makemkv.keyNotRemoved, makemkv.registryNotRestored, makemkv.registryFailed); empty when all went well.
    func recover(_ workDirectories: [String]) -> [BroMessage]
}
