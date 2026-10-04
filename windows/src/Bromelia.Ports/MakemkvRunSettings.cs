using System;
using System.Collections.Generic;
using System.Threading.Tasks;
using Bromelia.Domain;
using Bromelia.Foundation;

namespace Bromelia.Ports;

/// <summary>One makemkvcon run's settings: settings.conf (SettingsLayers.merge), the generated profile
/// (ProfileXml.render; none: no profile file), the user's MakeMKV data folder, and the run's own folder (the job's
/// home/: HOME, the profile file).</summary>
public sealed record MakemkvRunSettings(
    IReadOnlyDictionary<string, string> Settings,
    string? ProfileXml,
    string DataDir,
    string WorkDirectory);
