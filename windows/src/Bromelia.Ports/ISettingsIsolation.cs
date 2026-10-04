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
}
