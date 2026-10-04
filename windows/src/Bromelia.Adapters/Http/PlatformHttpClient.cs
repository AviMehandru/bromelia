using System;
using System.Collections.Generic;
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
/// redirects, gives up after the timeout (60 s), and says User-Agent: Bromelia unless the request sets one.</summary>
public sealed class PlatformHttpClient : IHttpClient
{
    static readonly System.Net.Http.HttpClient Client = new(new SocketsHttpHandler
    {
        AllowAutoRedirect = true,
        PooledConnectionLifetime = TimeSpan.FromMinutes(5),
    }) { Timeout = Timeout.InfiniteTimeSpan };

    readonly TimeSpan _timeout;

    public PlatformHttpClient(Duration? timeout = null) { _timeout = TimeSpan.FromSeconds(timeout?.Seconds ?? 60); }

    public async Task<HttpResponse> Send(HttpRequestSpec request, CancellationToken cancel)
    {
        using var message = new HttpRequestMessage(new HttpMethod(request.Method), request.Url);
        if (request.Body is { } body) message.Content = new ByteArrayContent(body);
        foreach (var (name, value) in request.Headers.Select(h => (h.Key, h.Value)))
            if (!message.Headers.TryAddWithoutValidation(name, value)) message.Content?.Headers.TryAddWithoutValidation(name, value);
        if (!request.Headers.Any(h => string.Equals(h.Key, "User-Agent", StringComparison.OrdinalIgnoreCase)))
            message.Headers.TryAddWithoutValidation("User-Agent", "Bromelia");
        using var timeout = System.Threading.CancellationTokenSource.CreateLinkedTokenSource(cancel.Token);
        timeout.CancelAfter(_timeout);
        try
        {
            using var response = await Client.SendAsync(message, timeout.Token).ConfigureAwait(false);
            var bytes = await response.Content.ReadAsByteArrayAsync(timeout.Token).ConfigureAwait(false);
            var headers = response.Headers.Concat(response.Content.Headers)
                .SelectMany(h => h.Value.Select(v => new KeyValuePair<string, string>(h.Key, v))).ToList();
            return new HttpResponse((int)response.StatusCode, headers, bytes);
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
    }

    static BroFailure Failed(string reason) =>
        new(new BroMessage(MessageCode.HttpFailed, Severity.Error, ("reason", JsonValue.Of(reason))).ToError());
}
