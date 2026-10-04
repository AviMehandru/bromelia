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

    void Release();
}
