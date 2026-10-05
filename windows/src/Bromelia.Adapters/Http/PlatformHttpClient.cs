using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Net.Http;
using System.Threading;
using System.Threading.Tasks;
using Bromelia.Domain;
using Bromelia.Foundation;
using Bromelia.Ports;
using CancellationToken = Bromelia.Foundation.CancellationToken;

namespace Bromelia.Adapters;

/// <summary>HttpClient on .NET's HttpClient (plan §6, §9; shared/fixtures/adapters/http-client.cases.json). Any status
/// is an answer; no answer is http.failed with the system's reason; a cancelled call is job.cancelled. Follows
/// redirects within the same origin only (the same host and port; http to https on the same host too): a redirect
/// elsewhere is the answer, so a request's headers (an API key) never go to a host the request didn't name. Bodies are
/// read up to <see cref="MaxBody"/> bytes; a larger one is http.failed. Gives up after the timeout (60 s), and says
/// User-Agent: Bromelia unless the request sets one. Errors never quote the URL: it can hold a secret.</summary>
public sealed class PlatformHttpClient : IHttpClient
{
    /// <summary>The largest answer read (16 MiB).</summary>
    public const int MaxBody = 16 * 1024 * 1024;

    const int MaxRedirects = 10;

    static readonly System.Net.Http.HttpClient Client = new(new SocketsHttpHandler
    {
        AllowAutoRedirect = false,
        PooledConnectionLifetime = TimeSpan.FromMinutes(5),
    }) { Timeout = Timeout.InfiniteTimeSpan };

    readonly TimeSpan _timeout;

    public PlatformHttpClient(Duration? timeout = null) { _timeout = TimeSpan.FromSeconds(timeout?.Seconds ?? 60); }

    public async Task<HttpResponse> Send(HttpRequestSpec request, CancellationToken cancel)
    {
        if (!Uri.TryCreate(request.Url, UriKind.Absolute, out var uri) || (uri.Scheme != Uri.UriSchemeHttp && uri.Scheme != Uri.UriSchemeHttps))
            throw Failed("not a valid http(s) URL");
        using var timeout = System.Threading.CancellationTokenSource.CreateLinkedTokenSource(cancel.Token);
        timeout.CancelAfter(_timeout);
        var method = request.Method;
        var body = request.Body;
        try
        {
            for (var hops = 0; ; hops++)
            {
                using var response = await Client.SendAsync(Message(request, method, uri, body), HttpCompletionOption.ResponseHeadersRead, timeout.Token)
                    .ConfigureAwait(false);
                var status = (int)response.StatusCode;
                if (status is 301 or 302 or 303 or 307 or 308 && response.Headers.Location is { } location && hops < MaxRedirects)
                {
                    var next = location.IsAbsoluteUri ? location : new Uri(uri, location);
                    if (SameOrigin(uri, next))
                    {
                        if (status == 303 || (status is 301 or 302 && method == "POST"))
                        {
                            method = "GET";
                            body = null;
                        }
                        uri = next;
                        continue;
                    }
                }
                var bytes = await ReadCapped(response, timeout.Token).ConfigureAwait(false);
                var headers = response.Headers.Concat(response.Content.Headers)
                    .SelectMany(h => h.Value.Select(v => new KeyValuePair<string, string>(h.Key, v))).ToList();
                return new HttpResponse(status, headers, bytes);
            }
        }
        catch (OperationCanceledException) when (cancel.IsCancelled)
        {
            throw new BroFailure(new BroError(MessageCode.Wire(MessageCode.JobCancelled)));
        }
        catch (OperationCanceledException)
        {
            throw Failed($"no answer within {_timeout.TotalSeconds:0} s");
        }
        catch (HttpRequestException e)
        {
            throw Failed(e.InnerException?.Message ?? e.Message);
        }
        catch (IOException e)
        {
            throw Failed(e.Message);
        }
    }

    static HttpRequestMessage Message(HttpRequestSpec request, string method, Uri uri, byte[]? body)
    {
        var message = new HttpRequestMessage(new HttpMethod(method), uri);
        if (body is not null) message.Content = new ByteArrayContent(body);
        foreach (var (name, value) in request.Headers.Select(h => (h.Key, h.Value)))
        {
            if (body is null && name.StartsWith("Content-", StringComparison.OrdinalIgnoreCase)) continue;
            if (!message.Headers.TryAddWithoutValidation(name, value)) message.Content?.Headers.TryAddWithoutValidation(name, value);
        }
        if (!request.Headers.Any(h => string.Equals(h.Key, "User-Agent", StringComparison.OrdinalIgnoreCase)))
            message.Headers.TryAddWithoutValidation("User-Agent", "Bromelia");
        return message;
    }

    /// <summary>Whether a redirect from <paramref name="from"/> may go to <paramref name="to"/>: the same host and port,
    /// or http to https on the same host with the default ports.</summary>
    internal static bool SameOrigin(Uri from, Uri to)
    {
        if (!string.Equals(from.Host, to.Host, StringComparison.OrdinalIgnoreCase)) return false;
        if (from.Scheme == to.Scheme) return from.Port == to.Port;
        return from.Scheme == Uri.UriSchemeHttp && to.Scheme == Uri.UriSchemeHttps && from.IsDefaultPort && to.IsDefaultPort;
    }

    static async Task<byte[]> ReadCapped(HttpResponseMessage response, System.Threading.CancellationToken token)
    {
        if (response.Content.Headers.ContentLength is > MaxBody) throw Failed($"the answer is larger than {MaxBody / 1024 / 1024} MiB");
        await using var stream = await response.Content.ReadAsStreamAsync(token).ConfigureAwait(false);
        var buffer = new MemoryStream();
        var chunk = new byte[81920];
        int n;
        while ((n = await stream.ReadAsync(chunk, token).ConfigureAwait(false)) > 0)
        {
            if (buffer.Length + n > MaxBody) throw Failed($"the answer is larger than {MaxBody / 1024 / 1024} MiB");
            buffer.Write(chunk, 0, n);
        }
        return buffer.ToArray();
    }

    static BroFailure Failed(string reason) =>
        new(new BroMessage(MessageCode.HttpFailed, Severity.Error, ("reason", JsonValue.Of(reason))).ToError());
}
