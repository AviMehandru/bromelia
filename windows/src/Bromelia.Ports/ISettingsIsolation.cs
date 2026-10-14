using System;
using System.Collections.Generic;
using System.Threading.Tasks;
using Bromelia.Domain;
using Bromelia.Foundation;

namespace Bromelia.Ports;

/// <summary>Gives each makemkvcon run its own settings.</summary>
public interface ISettingsIsolation
{
    IIsolationLease Prepare(MakemkvRunSettings settings);

    /// <summary>At startup, for the runs the engine never released (it stopped while they ran): a HOME folder loses the
    /// registration key line of its settings.conf; the registry gets back the user's values a crash left in the snapshot
    /// file. <paramref name="workDirectories"/> are those runs' folders (the unfinished jobs' home/). What it couldn't do,
    /// as warnings (makemkv.keyNotRemoved, makemkv.registryNotRestored, makemkv.registryFailed); empty when all went
    /// well.</summary>
    IReadOnlyList<BroMessage> Recover(IReadOnlyList<string> workDirectories);
}
