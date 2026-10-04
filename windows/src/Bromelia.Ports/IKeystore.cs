using System;
using System.Collections.Generic;
using System.Threading.Tasks;
using Bromelia.Domain;
using Bromelia.Foundation;

namespace Bromelia.Ports;

/// <summary>Secrets (config SecretRef names), outside the configuration.</summary>
public interface IKeystore
{
    /// <summary>None when there is no such secret.</summary>
    string? Get(string name);

    void Set(string name, string value);

    void Remove(string name);
}
