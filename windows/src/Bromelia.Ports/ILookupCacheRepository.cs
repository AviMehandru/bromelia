using System;
using System.Collections.Generic;
using System.Threading.Tasks;
using Bromelia.Domain;
using Bromelia.Foundation;

namespace Bromelia.Ports;

/// <summary>Online lookup answers (lookup_cache).</summary>
public interface ILookupCacheRepository
{
    /// <summary>The answer, while it hasn't expired.</summary>
    Task<HttpResponse?> Get(string provider, string key);

    /// <summary>key: the request without credentials.</summary>
    Task Put(string provider, string key, HttpResponse response, Instant expiresAt);
}
