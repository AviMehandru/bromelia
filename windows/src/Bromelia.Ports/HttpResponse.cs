using System;
using System.Collections.Generic;
using System.Threading.Tasks;
using Bromelia.Domain;
using Bromelia.Foundation;

namespace Bromelia.Ports;

/// <summary>An HTTP answer: status, headers in order, body.</summary>
public sealed record HttpResponse(
    int Status,
    IReadOnlyList<KeyValuePair<string, string>> Headers,
    byte[] Body);
