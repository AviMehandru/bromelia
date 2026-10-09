using System;
using System.Collections.Generic;
using System.Threading.Tasks;
using Bromelia.Domain;
using Bromelia.Foundation;

namespace Bromelia.Ports;

/// <summary>One makemkvcon launch's settings: a HOME folder, or registry values until makemkvcon has read
/// them.</summary>
public interface IIsolationLease
{
    /// <summary>Variables to add to the process's environment (HOME).</summary>
    IReadOnlyDictionary<string, string> Environment();

    /// <summary>The profile file for --profile, when there is one.</summary>
    string? ProfilePath();

    /// <summary>makemkvcon has read its settings (its first output line).</summary>
    void FirstOutput();

    /// <summary>Gives the settings back once the run has ended: why they couldn't all be cleaned up
    /// (makemkv.keyNotRemoved: the registration key left in the job's folder; makemkv.registryNotRestored: the user's
    /// registry values not back yet), or null.</summary>
    BroMessage? Release();
}
