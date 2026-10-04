using System;
using System.Collections.Generic;
using System.Threading.Tasks;
using Bromelia.Domain;
using Bromelia.Foundation;

namespace Bromelia.Ports;

/// <summary>The kv table: small JSON values.</summary>
public interface IKeyValueRepository
{
    Task<JsonValue?> Get(string key);

    Task Set(string key, JsonValue value);
}
