using Bromelia.Domain;
using Bromelia.Ports;

namespace Bromelia.Engine;

/// <summary>How a step gets what a makemkvcon run needs (implemented by MakemkvService, which holds the registration
/// key and the user's MakeMKV data folder): the settings, the profile file and the switches of a profile, run from
/// the job's folder, under the configuration the job started with (its makemkv.settings, registration key and
/// limits.stallTimeoutMinutes).</summary>
public interface IMakemkvSetup
{
    /// <summary><paramref name="selectionOverride"/>: app_DefaultSelectionString for this run (hand-picked tracks:
    /// +sel:all); null keeps the profile's.</summary>
    MakemkvInvocation Invocation(Config config, EffectiveProfile profile, string? selectionOverride, string jobDirectory);
}
