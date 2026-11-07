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

    /// <summary>Why the run's settings may not hold, known once they are in place: makemkv.guiRunning (Windows: the
    /// MakeMKV GUI was open while the registry held the run's values, and it may write its own back), or null
    /// .</summary>
    BroMessage? Warning();

    /// <summary>makemkvcon has read its settings (its first output line).</summary>
    void FirstOutput();

    /// <summary>Gives the settings back once the run has ended: why they couldn't all be cleaned up
    /// (makemkv.keyNotRemoved: the registration key left in the job's folder; makemkv.registryNotRestored: the user's
    /// registry values not back yet), or null.</summary>
    BroMessage? Release();
}
