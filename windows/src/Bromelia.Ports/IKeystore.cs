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

    /// <summary>True when the secrets are in the system's store (Keychain, Credential Manager, Secret Service), false
    /// when they are files in the secrets folder because there is none (Docker, a service without a logon).</summary>
    bool IsSystemStore();
}
