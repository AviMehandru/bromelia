using System;
using System.Collections.Generic;
using System.Threading.Tasks;
using Bromelia.Domain;
using Bromelia.Foundation;

namespace Bromelia.Ports;

/// <summary>Sends requests that Domain built.</summary>
public interface IHttpClient
{
    /// <summary>Any status is an answer; fails when there is none.</summary>
    Task<HttpResponse> Send(HttpRequestSpec request, CancellationToken cancel);
}
