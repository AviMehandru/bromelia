using System.Collections.Generic;

namespace Bromelia.Foundation;

/// <summary>A request built by Domain and sent by the HttpClient port: method, URL, headers in order, body.</summary>
public sealed record HttpRequestSpec(string Method, string Url, IReadOnlyList<KeyValuePair<string, string>> Headers, byte[]? Body = null)
{
    public bool Equals(HttpRequestSpec? other) =>
        other is not null && Method == other.Method && Url == other.Url
        && System.Linq.Enumerable.SequenceEqual(Headers, other.Headers)
        && (Body is null ? other.Body is null : other.Body is not null && System.Linq.Enumerable.SequenceEqual(Body, other.Body));

    public override int GetHashCode() => Url.GetHashCode();
}
