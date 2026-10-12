import BroDomain
import BroFoundation
import BroPorts

/// How a step gets what a makemkvcon run needs (implemented by MakemkvService, which holds the registration key and
/// the user's MakeMKV data folder): the settings, the profile file and the switches of a profile, run from the job's
/// folder.
public protocol MakemkvSetup: Sendable {
    /// `selectionOverride`: app_DefaultSelectionString for this run (hand-picked tracks: +sel:all); nil keeps the
    /// profile's.
    func invocation(_ profile: EffectiveProfile, selectionOverride: String?, jobDirectory: String) throws(BroError) -> MakemkvInvocation
}
